# Radxa A733 KVM VGIC

This note records a Radxa Cubie A7S / Allwinner A733 host issue where TenBox
cannot create an arm64 KVM VM because the host kernel does not expose a usable
in-kernel VGIC device.

## Symptom

TenBox runtime fails before the guest starts:

```text
[INFO]  kvm: KVM_CREATE_DEVICE(VGIC_V3) unavailable: No such device
[WARN]  kvm: VGICv3 unavailable, falling back to VGICv2
[ERROR] kvm: KVM_CREATE_DEVICE(VGIC_V2) failed: No such device
[ERROR] kvm: neither VGICv3 nor VGICv2 could be created
Failed to create VM
```

The host may still show `/dev/kvm` and successful KVM initialization:

```text
kvm [1]: IPA Size Limit: 40 bits
kvm [1]: VHE mode initialized successfully
```

That is not enough for TenBox. On arm64, TenBox requires the kernel's in-kernel
VGIC device, created with `KVM_CREATE_DEVICE(KVM_DEV_TYPE_ARM_VGIC_V3)`.

## Diagnosis

Run a small root-only probe or use QEMU/libvirt to confirm that VGIC device
creation fails independently of TenBox. The important result is:

```text
KVM_CHECK_EXTENSION DEVICE_CTRL = 1
KVM_CHECK_EXTENSION IRQCHIP     = 0
KVM_CREATE_DEVICE ARM_VGIC_V3   = -1 errno=19 No such device
KVM_CREATE_DEVICE ARM_VGIC_V2   = -1 errno=19 No such device
```

On the affected A7S image, the live device tree has a GICv3 node but no
maintenance interrupt:

```sh
for f in compatible interrupts reg interrupt-parent; do
  [ -f /proc/device-tree/interrupt-controller@3400000/$f ] || continue
  printf '%s: ' "$f"
  if [ "$f" = compatible ]; then
    tr '\000' ' ' </proc/device-tree/interrupt-controller@3400000/$f
  else
    od -An -tx4 -v /proc/device-tree/interrupt-controller@3400000/$f
  fi
  echo
done
```

Broken output has no `interrupts` property under
`/proc/device-tree/interrupt-controller@3400000`.

Linux's `irq-gic-v3` KVM setup calls `irq_of_parse_and_map(node, 0)` for the
VGIC maintenance interrupt. If the GIC node has no `interrupts` property, KVM
does not receive GIC KVM info, so the VGIC device is not registered for user
space and `KVM_CREATE_DEVICE(VGIC_V3)` returns `ENODEV`.

## Temporary Host Fix

This is a DTB patch for the currently installed kernel. It should be replaced
by a proper kernel/DTS package update when available. The procedure below has
been validated on `6.6.98-2-aw2511`; upgrade A7S hosts to that kernel first
rather than patching the older `6.6.98-1-aw2511` DTB in place.

For Radxa Cubie A7A / A7S / A7Z hosts, the packaged helper script can apply
the same DTB patch automatically:

```sh
curl -fsSL https://tenbox.ai/fix-radxa-a733-vgic.sh | bash
sudo reboot
```

The script backs up the target DTB next to the original file using a
`.bak-tenbox-vgic-<timestamp>` suffix before writing the patched DTB. It can be
run in check-only mode with:

```sh
curl -fsSL https://tenbox.ai/fix-radxa-a733-vgic.sh | bash -s -- --check
```

Example for `6.6.98-2-aw2511` on Cubie A7S:

```sh
DTB=/usr/lib/linux-image-6.6.98-2-aw2511/allwinner/sun60i-a733-cubie-a7s.dtb
WORK=/tmp/tenbox-dtb-fix

sudo cp -a "$DTB" "$DTB.bak-tenbox-vgic"
rm -rf "$WORK"
mkdir -p "$WORK"
sudo dtc -I dtb -O dts -o "$WORK/orig.dts" "$DTB"
```

Edit `"$WORK/orig.dts"` and add the VGIC maintenance interrupt to the GICv3
node:

```dts
interrupt-controller@3400000 {
        compatible = "arm,gic-v3";
        interrupts = <0x01 0x09 0x04>;
        #interrupt-cells = <0x03>;
        #address-cells = <0x00>;
        interrupt-controller;
        reg = <0x00 0x3400000 0x00 0x10000 0x00 0x3460000 0x00 0xff004>;
        interrupt-parent = <0x02>;
};
```

Then compile and reboot:

```sh
dtc -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/orig.dts"
sudo cp "$WORK/patched.dtb" "$DTB"
sudo chmod 0644 "$DTB"
sudo reboot
```

`<0x01 0x09 0x04>` is `GIC_PPI 9` with `IRQ_TYPE_LEVEL_HIGH`, i.e. architectural
INTID 25, the common VGIC maintenance interrupt used by arm64 KVM.

To roll back:

```sh
DTB=/usr/lib/linux-image-6.6.98-2-aw2511/allwinner/sun60i-a733-cubie-a7s.dtb
sudo cp "$DTB.bak-tenbox-vgic" "$DTB"
sudo reboot
```

## Expected Result

After reboot, dmesg should include:

```text
kvm [1]: GICv3: no GICV resource entry
kvm [1]: disabling GICv2 emulation
kvm [1]: GIC system register CPU interface enabled
kvm [1]: vgic interrupt IRQ9
kvm [1]: VHE mode initialized successfully
```

The probe should change to:

```text
KVM_CHECK_EXTENSION IRQCHIP   = 1
KVM_CREATE_DEVICE ARM_VGIC_V3 = 0 Success
```

TenBox should then get past VM creation:

```text
[INFO]  kvm: aarch64 VM created (..., vgic=v3)
[INFO]  kvm: in-kernel VGICv3 initialized
```

## Upstream Fix

The proper fix belongs in the Radxa/Allwinner A733 DTS source for Cubie A7S,
not in TenBox. Add the maintenance interrupt to the `gic` node, using symbolic
bindings in source form:

```dts
gic: interrupt-controller@3400000 {
        compatible = "arm,gic-v3";
        interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH>;
        #interrupt-cells = <3>;
        #address-cells = <0>;
        interrupt-controller;
        reg = <0x0 0x03400000 0x0 0x10000>,
              <0x0 0x03460000 0x0 0xff004>;
};
```

Package updates can overwrite the manual DTB patch, so re-check the live
device tree and KVM probe after upgrading `linux-image-*-aw2511`.
