import SwiftUI

@main
struct Grid0RelayApp: App {
    @StateObject private var relay = RelayModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(relay)
                .frame(minWidth: 720, minHeight: 600)
        }
        .windowResizability(.contentMinSize)
    }
}
