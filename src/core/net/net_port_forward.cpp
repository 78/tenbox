// NetBackend: host-to-guest TCP port forwarding.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/vmm/types.h"

extern "C" {
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"
}

#include <uv.h>
#include <cstring>


// ============================================================
// Setup / teardown
// ============================================================

std::vector<uint16_t> NetBackend::SetupPortForwards() {
    std::vector<uint16_t> failed_ports;

    for (auto& pf : port_forwards_) {
        SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == SOCK_INVALID) {
            LOG_ERROR("Port forward: failed to create listener for port %u", pf.host_port);
            failed_ports.push_back(pf.host_port);
            continue;
        }
#ifdef _WIN32
        int exclusive = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   SOCK_CCAST(&exclusive), sizeof(exclusive));
#else
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   SOCK_CCAST(&reuse), sizeof(reuse));
#endif
        SOCK_SETNONBLOCK(s);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(pf.host_ip == "0.0.0.0" ? INADDR_ANY : INADDR_LOOPBACK);
        addr.sin_port = htons(pf.host_port);

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR ||
            listen(s, 5) == SOCK_ERR) {
            LOG_ERROR("Port forward: failed to bind/listen on port %u", pf.host_port);
            SOCK_CLOSE(s);
            failed_ports.push_back(pf.host_port);
            continue;
        }

        pf.listener = static_cast<uintptr_t>(s);

        pf.listener_poll.Init(&loop_, s);
        pf.listener_poll.Start(UV_READABLE, [](uv_poll_t* h, int status, int) {
            if (status < 0) return;
            auto* pf = static_cast<PfEntry*>(h->data);
            pf->backend->OnPfListenerReadable(pf);
        }, &pf);

        LOG_INFO("Port forward: %s:%u -> guest:%u",
                 pf.host_ip == "0.0.0.0" ? "0.0.0.0" : "127.0.0.1", pf.host_port, pf.guest_port);
    }

    return failed_ports;
}

void NetBackend::TeardownPortForwards() {
    for (auto& pf : port_forwards_) {
        pf.listener_poll.Close();
        if (pf.listener != ~(uintptr_t)0) {
            SOCK_CLOSE(static_cast<SocketHandle>(pf.listener));
            pf.listener = ~(uintptr_t)0;
        }
        for (auto& c : pf.conns) {
            c.poll.Close();
            if (c.host_sock != ~(uintptr_t)0) {
                SOCK_CLOSE(static_cast<SocketHandle>(c.host_sock));
                c.host_sock = ~(uintptr_t)0;
            }
            if (c.guest_pcb) {
                auto* pcb = static_cast<struct tcp_pcb*>(c.guest_pcb);
                tcp_arg(pcb, nullptr);
                tcp_recv(pcb, nullptr);
                tcp_err(pcb, nullptr);
                tcp_abort(pcb);
                c.guest_pcb = nullptr;
            }
        }
    }
    auto all_polls_closed = [this]() {
        for (const auto& pf : port_forwards_) {
            if (pf.listener_poll.inited() && !pf.listener_poll.closed())
                return false;
            for (const auto& c : pf.conns) {
                if (c.poll.inited() && !c.poll.closed())
                    return false;
            }
        }
        return true;
    };
    while (!all_polls_closed())
        uv_run(&loop_, UV_RUN_NOWAIT);
    for (auto& pf : port_forwards_)
        pf.conns.clear();
    port_forwards_.clear();
}

void NetBackend::CheckPendingUpdates() {
    std::optional<std::vector<PortForward>> update;
    PortForwardCallback cb;
    {
        std::lock_guard<std::mutex> lock(pf_update_mutex_);
        if (pending_pf_update_) {
            update = std::move(pending_pf_update_);
            pending_pf_update_.reset();
            cb = std::move(pf_update_cb_);
        }
    }
    if (update) {
        TeardownPortForwards();
        for (const auto& f : *update) {
            port_forwards_.emplace_back();
            auto& pf = port_forwards_.back();
            pf.backend = this;
            pf.host_port = f.host_port;
            pf.guest_port = f.guest_port;
            pf.host_ip = f.host_ip;
        }
        auto failed = SetupPortForwards();
        LOG_INFO("Port forwards updated (%zu entries, %zu failed)",
                 update->size(), failed.size());

        if (cb) cb(std::move(failed));
    }
}

// ============================================================
// PfConn data relay
// ============================================================

void NetBackend::DrainPfToGuest(PfEntry::Conn& conn) {
    if (conn.pending_to_guest.empty() || !conn.guest_pcb || !conn.guest_connected)
        return;

    auto* pcb = static_cast<struct tcp_pcb*>(conn.guest_pcb);
    uint16_t avail = tcp_sndbuf(pcb);
    uint16_t to_write = static_cast<uint16_t>(
        std::min<size_t>(avail, conn.pending_to_guest.size()));
    if (to_write > 0) {
        tcp_write(pcb, conn.pending_to_guest.data(), to_write, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        conn.pending_to_guest.erase(
            conn.pending_to_guest.begin(),
            conn.pending_to_guest.begin() + to_write);
    }
}

void NetBackend::DrainPfToHost(PfEntry::Conn& conn) {
    if (conn.pending_to_host.empty()) return;
    if (conn.host_sock == ~(uintptr_t)0) {
        conn.pending_to_host.clear();
        return;
    }

    SocketHandle s = static_cast<SocketHandle>(conn.host_sock);
    int sent = send(s, SOCK_CCAST(conn.pending_to_host.data()),
                    static_cast<int>(conn.pending_to_host.size()), 0);

    if (sent > 0) {
        conn.pending_to_host.erase(
            conn.pending_to_host.begin(),
            conn.pending_to_host.begin() + sent);
    } else if (sent == SOCK_ERR) {
        int err = SOCK_ERRNO;
        if (err != SOCK_WOULDBLOCK) {
            TeardownPfConn(conn);
        }
    }
}

// ============================================================
// PfConn poll event handler
// ============================================================

void NetBackend::OnPfConnPollEvent(PfEntry::Conn* c, int status, int events) {
    if (status < 0 || c->host_sock == ~(uintptr_t)0) {
        TeardownPfConn(*c);
        return;
    }

    SocketHandle s = static_cast<SocketHandle>(c->host_sock);

    if (!c->pending_to_host.empty() && (events & UV_WRITABLE)) {
        DrainPfToHost(*c);
    }

    if (events & UV_READABLE) {
        DrainPfToGuest(*c);
        if (c->pending_to_guest.empty()) {
            char buf[4096];
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                TeardownPfConn(*c);
                return;
            }
            c->pending_to_guest.insert(c->pending_to_guest.end(), buf, buf + n);
            DrainPfToGuest(*c);
        }
    }

    if (c->host_sock != ~(uintptr_t)0)
        UpdatePfConnPoll(*c);
    else
        c->poll.Stop();
}

// ============================================================
// Listener accept handler
// ============================================================

void NetBackend::OnPfListenerReadable(PfEntry* pf) {
    SocketHandle ls = static_cast<SocketHandle>(pf->listener);
    SocketHandle cs = accept(ls, nullptr, nullptr);
    if (cs == SOCK_INVALID) return;
    SOCK_SETNONBLOCK(cs);

    struct tcp_pcb* pcb = tcp_new();
    ip_addr_t guest_addr;
    IP4_ADDR(ip_2_ip4(&guest_addr), 10, 0, 2, 15);

    pf->conns.emplace_back();
    auto* conn_ptr = &pf->conns.back();
    conn_ptr->backend = pf->backend;
    conn_ptr->host_sock = static_cast<uintptr_t>(cs);
    conn_ptr->guest_pcb = pcb;
    tcp_arg(pcb, conn_ptr);
    tcp_recv(pcb, [](void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) -> err_t {
        auto* c = static_cast<PfEntry::Conn*>(arg);
        if (!c) {
            if (p) pbuf_free(p);
            return ERR_OK;
        }
        if (!p) {
            c->backend->TeardownPfConn(*c);
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_close(pcb);
            c->guest_pcb = nullptr;
            return ERR_OK;
        }
        std::vector<uint8_t> data(p->tot_len);
        pbuf_copy_partial(p, data.data(), p->tot_len, 0);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        c->pending_to_host.insert(c->pending_to_host.end(),
                                  data.begin(), data.end());
        c->backend->DrainPfToHost(*c);
        c->backend->UpdatePfConnPoll(*c);
        return ERR_OK;
    });
    tcp_err(pcb, [](void* arg, err_t err) {
        auto* c = static_cast<PfEntry::Conn*>(arg);
        if (!c) return;
        c->guest_pcb = nullptr;
        c->backend->TeardownPfConn(*c);
    });

    tcp_connect(pcb, &guest_addr, pf->guest_port,
        [](void* arg, struct tcp_pcb* pcb, err_t err) -> err_t {
        auto* c = static_cast<PfEntry::Conn*>(arg);
        if (!c) return ERR_OK;
        c->guest_connected = true;
        if (!c->pending_to_guest.empty()) {
            tcp_write(pcb, c->pending_to_guest.data(),
                      static_cast<u16_t>(c->pending_to_guest.size()),
                      TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            c->pending_to_guest.clear();
        }
        c->backend->UpdatePfConnPoll(*c);
        return ERR_OK;
    });

    UpdatePfConnPoll(*conn_ptr);
}
