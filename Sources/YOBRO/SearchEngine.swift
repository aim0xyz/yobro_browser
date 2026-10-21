import Foundation

/// The provider that receives typed text which is not an address.
enum SearchEngine: String, CaseIterable, Identifiable {
    case duckDuckGo, google, bing, ecosia, startpage, kagi

    static let defaultsKey = "YOBRO.searchEngine"

    /// The chosen engine. A missing or unknown value falls back to DuckDuckGo.
    static var current: SearchEngine {
        get { UserDefaults.standard.string(forKey: defaultsKey).flatMap(SearchEngine.init(rawValue:)) ?? .duckDuckGo }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: defaultsKey) }
    }

    var id: String { rawValue }

    var name: String {
        switch self {
        case .duckDuckGo: return "DuckDuckGo"
        case .google: return "Google"
        case .bing: return "Bing"
        case .ecosia: return "Ecosia"
        case .startpage: return "Startpage"
        case .kagi: return "Kagi"
        }
    }

    func url(for query: String) -> URL {
        var components = URLComponents()
        components.scheme = "https"
        switch self {
        case .duckDuckGo: components.host = "duckduckgo.com"; components.path = "/"
        case .google: components.host = "www.google.com"; components.path = "/search"
        case .bing: components.host = "www.bing.com"; components.path = "/search"
        case .ecosia: components.host = "www.ecosia.org"; components.path = "/search"
        case .startpage: components.host = "www.startpage.com"; components.path = "/sp/search"
        case .kagi: components.host = "kagi.com"; components.path = "/search"
        }
        components.queryItems = [URLQueryItem(name: self == .startpage ? "query" : "q", value: query)]
        return components.url!
    }
}
