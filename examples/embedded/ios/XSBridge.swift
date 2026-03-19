import Foundation

@_silgen_name("xs_eval_cstr")
func xs_eval_cstr(_ src: UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>?

@_silgen_name("free")
func xs_free(_ p: UnsafeMutablePointer<CChar>?)

public enum XS {
    public static func eval(_ source: String) -> String {
        guard let raw = source.withCString({ xs_eval_cstr($0) }) else {
            return ""
        }
        defer { xs_free(raw) }
        return String(cString: raw)
    }
}

#if DEBUG
@inline(never)
public func _xs_smoke_test() {
    print(XS.eval("[1, 2, 3].fold(0, |a, b| a + b)"))
}
#endif
