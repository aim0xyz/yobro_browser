import Foundation
import Security
import CryptoKit

/// What a user needs in order to judge a certificate.
struct CertificateSummary {
    var subject: String
    var issuer: String
    var validity: String
    /// SHA-256 over the DER encoding, formatted in pairs. This is the value to
    /// compare against the server, not the names above.
    var fingerprint: String
}

/// Session-only certificate exceptions for local development hosts.
///
/// YOBRO hands server trust to WebKit and offers no way past a rejected
/// certificate, which is right for real websites: the "continue anyway" button is
/// the classic path into a machine-in-the-middle attack. The cost was that a
/// local HTTPS dev server, a router page or an internal host with a self-signed
/// certificate was simply unreachable.
///
/// The compromise is that only hosts which cannot be a public website may be
/// excepted, the user has to see the fingerprint first, and nothing is written to
/// disk — the exception dies with the process.
@MainActor
final class CertificateTrustStore: ObservableObject {
    /// Host to accepted leaf fingerprint. Keyed by both so a swapped certificate
    /// asks again instead of inheriting the previous approval.
    private var accepted: [String: String] = [:]

    func isAccepted(host: String, fingerprint: String) -> Bool {
        accepted[host.lowercased()] == fingerprint
    }

    func accept(host: String, fingerprint: String) {
        accepted[host.lowercased()] = fingerprint
    }

    func forget(host: String) {
        accepted.removeValue(forKey: host.lowercased())
    }

    var exceptionCount: Int { accepted.count }

    // MARK: - Eligibility

    /// True for hosts that cannot be a public website: loopback, link-local,
    /// the private IPv4 ranges and the reserved local-only suffixes.
    nonisolated static func isLocal(_ host: String) -> Bool {
        let name = host.lowercased()
        if name == "localhost" || name == "::1" { return true }
        if [".localhost", ".local", ".internal", ".test", ".home.arpa"].contains(where: { name.hasSuffix($0) }) { return true }
        return isPrivateIPv4(name)
    }

    private nonisolated static func isPrivateIPv4(_ host: String) -> Bool {
        let parts = host.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        let octets = parts.compactMap { UInt8($0) }
        guard octets.count == 4 else { return false }
        switch (octets[0], octets[1]) {
        case (10, _): return true                       // 10.0.0.0/8
        case (127, _): return true                      // loopback
        case (169, 254): return true                    // link-local
        case (192, 168): return true                    // 192.168.0.0/16
        case (172, 16...31): return true                // 172.16.0.0/12
        default: return false
        }
    }

    // MARK: - Certificate inspection

    nonisolated static func summary(_ trust: SecTrust) -> CertificateSummary? {
        guard let certificate = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first else { return nil }
        let der = SecCertificateCopyData(certificate) as Data
        let digest = SHA256.hash(data: der)
        let fingerprint = digest.map { String(format: "%02X", $0) }
            .enumerated()
            .map { $0.offset > 0 && $0.offset % 2 == 0 ? " " + $0.element : $0.element }
            .joined()

        let subject = (SecCertificateCopySubjectSummary(certificate) as String?) ?? L("Unbekannt", "Unknown")
        return CertificateSummary(
            subject: subject,
            issuer: issuer(of: certificate) ?? L("Selbst ausgestellt oder unbekannt", "Self-issued or unknown"),
            validity: validity(of: certificate) ?? L("Gültigkeit unbekannt", "Validity unknown"),
            fingerprint: fingerprint
        )
    }

    /// The issuer arrives as a nested property tree. Everything is optional so an
    /// unexpected shape degrades to "unknown" rather than failing the dialog.
    private nonisolated static func issuer(of certificate: SecCertificate) -> String? {
        guard let values = SecCertificateCopyValues(certificate, [kSecOIDX509V1IssuerName] as CFArray, nil) as? [String: Any],
              let entry = values[kSecOIDX509V1IssuerName as String] as? [String: Any],
              let fields = entry[kSecPropertyKeyValue as String] as? [[String: Any]] else { return nil }
        // Prefer the common name, fall back to the organisation.
        let wanted = ["2.5.4.3", "2.5.4.10"]
        for oid in wanted {
            if let match = fields.first(where: { ($0[kSecPropertyKeyLabel as String] as? String) == oid }),
               let value = match[kSecPropertyKeyValue as String] as? String {
                return value
            }
        }
        return nil
    }

    private nonisolated static func validity(of certificate: SecCertificate) -> String? {
        let keys = [kSecOIDX509V1ValidityNotBefore, kSecOIDX509V1ValidityNotAfter] as CFArray
        guard let values = SecCertificateCopyValues(certificate, keys, nil) as? [String: Any] else { return nil }
        func date(_ oid: CFString) -> Date? {
            guard let entry = values[oid as String] as? [String: Any],
                  let seconds = entry[kSecPropertyKeyValue as String] as? NSNumber else { return nil }
            return Date(timeIntervalSinceReferenceDate: seconds.doubleValue)
        }
        guard let from = date(kSecOIDX509V1ValidityNotBefore), let until = date(kSecOIDX509V1ValidityNotAfter) else { return nil }
        let format = Date.FormatStyle(date: .abbreviated, time: .omitted)
        let expired = until < Date()
        let range = "\(from.formatted(format)) – \(until.formatted(format))"
        return expired ? range + " · " + L("abgelaufen", "expired") : range
    }
}
