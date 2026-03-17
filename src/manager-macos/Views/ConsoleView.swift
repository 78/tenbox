import SwiftUI
import AppKit

struct ConsoleView: View {
    @ObservedObject var session: VmSession

    @State private var inputText = ""

    var body: some View {
        VStack(spacing: 0) {
            ConsoleTextView(text: session.consoleText)

            Divider()

            HStack {
                TextField("Type command...", text: $inputText)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 12, design: .monospaced))
                    .onSubmit {
                        sendInput()
                    }
                Button("Send") {
                    sendInput()
                }
                .keyboardShortcut(.return, modifiers: [])
            }
            .padding(8)
        }
    }

    private func sendInput() {
        let text = inputText + "\n"
        inputText = ""
        session.sendConsoleInput(text)
    }
}

struct ConsoleTextView: NSViewRepresentable {
    let text: String

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSTextView.scrollableTextView()
        let textView = scrollView.documentView as! NSTextView
        textView.isEditable = false
        textView.isSelectable = true
        textView.isRichText = false
        textView.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        textView.textColor = .textColor
        textView.backgroundColor = .textBackgroundColor
        textView.textContainerInset = NSSize(width: 4, height: 4)
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        guard let textView = scrollView.documentView as? NSTextView else { return }
        let atBottom = isScrolledToBottom(scrollView)
        textView.string = text
        if atBottom {
            textView.scrollToEndOfDocument(nil)
        }
    }

    private func isScrolledToBottom(_ scrollView: NSScrollView) -> Bool {
        let contentView = scrollView.contentView
        let docHeight = scrollView.documentView?.frame.height ?? 0
        let clipHeight = contentView.bounds.height
        if docHeight <= clipHeight { return true }
        return contentView.bounds.origin.y >= docHeight - clipHeight - 20
    }
}
