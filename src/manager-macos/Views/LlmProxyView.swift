import SwiftUI

struct LlmProxySheet: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @State private var showAddSheet = false
    @State private var editMapping: LlmModelMapping?

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("LLM Proxy")
                    .font(.headline)
                Spacer()
                Button {
                    showAddSheet = true
                } label: {
                    Image(systemName: "plus")
                }
                .buttonStyle(.borderless)
            }
            .padding(.horizontal)
            .padding(.top)

            if appState.llmMappings.isEmpty {
                Text("No model mappings")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    ForEach(appState.llmMappings) { mapping in
                        HStack(spacing: 8) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(mapping.alias)
                                    .fontWeight(.medium)
                                Text("\(mapping.targetUrl) · \(mapping.model)")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button {
                                editMapping = mapping
                            } label: {
                                Image(systemName: "pencil")
                            }
                            .buttonStyle(.borderless)
                            Button(role: .destructive) {
                                appState.removeLlmMapping(alias: mapping.alias)
                            } label: {
                                Image(systemName: "minus.circle")
                            }
                            .buttonStyle(.borderless)
                        }
                    }
                }
            }

            Text("VMs can access LLM APIs via http://10.0.2.3/ with any API key. Model aliases map to your configured LLM backends, keeping API credentials safe on the host.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .padding(.horizontal)
                .padding(.bottom, 4)

            HStack {
                Button("Done") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
            }
            .padding()
        }
        .frame(width: 440, height: 380)
        .sheet(isPresented: $showAddSheet) {
            EditLlmMappingSheet(mode: .add)
        }
        .sheet(item: $editMapping) { mapping in
            EditLlmMappingSheet(mode: .edit(mapping))
        }
    }
}

struct EditLlmMappingSheet: View {
    enum Mode: Identifiable {
        case add
        case edit(LlmModelMapping)

        var id: String {
            switch self {
            case .add: return "add"
            case .edit(let m): return m.alias
            }
        }
    }

    let mode: Mode
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var alias = ""
    @State private var targetUrl = ""
    @State private var apiKey = ""
    @State private var model = ""
    @State private var apiType: LlmApiType = .openaiCompletions

    private var isEdit: Bool {
        if case .edit = mode { return true }
        return false
    }

    private var originalAlias: String? {
        if case .edit(let m) = mode { return m.alias }
        return nil
    }

    private var isValid: Bool {
        !alias.trimmingCharacters(in: .whitespaces).isEmpty &&
        !targetUrl.trimmingCharacters(in: .whitespaces).isEmpty &&
        !model.trimmingCharacters(in: .whitespaces).isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            Text(isEdit ? "Edit Mapping" : "Add Mapping")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            Form {
                TextField("Alias", text: $alias, prompt: Text("e.g. default"))
                    .disableAutocorrection(true)
                Picker("API Type", selection: $apiType) {
                    ForEach(LlmApiType.allCases) { type in
                        Text(type.displayName).tag(type)
                    }
                }
                TextField("Base URL", text: $targetUrl, prompt: Text("https://api.openai.com/v1"))
                    .disableAutocorrection(true)
                TextField("API Key", text: $apiKey, prompt: Text("sk-..."))
                    .disableAutocorrection(true)
                TextField("Model", text: $model, prompt: Text("e.g. gpt-4o"))
                    .disableAutocorrection(true)
            }
            .padding(.horizontal)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button(isEdit ? "Save" : "Add") { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
            .padding()
        }
        .frame(width: 400, height: 300)
        .onAppear {
            if case .edit(let m) = mode {
                alias = m.alias
                targetUrl = m.targetUrl
                apiKey = m.apiKey
                model = m.model
                apiType = m.apiType
            }
        }
    }

    private func save() {
        let mapping = LlmModelMapping(
            alias: alias.trimmingCharacters(in: .whitespaces),
            targetUrl: targetUrl.trimmingCharacters(in: .whitespaces),
            apiKey: apiKey.trimmingCharacters(in: .whitespaces),
            model: model.trimmingCharacters(in: .whitespaces),
            apiType: apiType
        )
        if isEdit {
            appState.updateLlmMapping(originalAlias: originalAlias!, mapping: mapping)
        } else {
            appState.addLlmMapping(mapping)
        }
        dismiss()
    }
}
