-- xs-diff: line diff via Myers' simple LCS, unified-diff format.

fn _lcs(a, b) {
    let n = a.len()
    let m = b.len()
    var dp = []
    for _ in 0..(n + 1) {
        var row = []
        for _ in 0..(m + 1) { row.push(0) }
        dp.push(row)
    }
    var i = 1
    while i <= n {
        var j = 1
        while j <= m {
            if a[i - 1] == b[j - 1] {
                dp[i][j] = dp[i - 1][j - 1] + 1
            } else {
                dp[i][j] = if dp[i - 1][j] > dp[i][j - 1] { dp[i - 1][j] } else { dp[i][j - 1] }
            }
            j = j + 1
        }
        i = i + 1
    }
    return dp
}

fn lines(a, b) {
    let la = a.split("\n")
    let lb = b.split("\n")
    let dp = _lcs(la, lb)
    var ops = []
    var i = la.len()
    var j = lb.len()
    while i > 0 or j > 0 {
        if i > 0 and j > 0 and la[i - 1] == lb[j - 1] {
            ops.push(#{op: "=", line: la[i - 1]})
            i = i - 1; j = j - 1
        } else if j > 0 and (i == 0 or dp[i][j - 1] >= dp[i - 1][j]) {
            ops.push(#{op: "+", line: lb[j - 1]})
            j = j - 1
        } else {
            ops.push(#{op: "-", line: la[i - 1]})
            i = i - 1
        }
    }
    ops.reverse()
    return ops
}

fn unified(a, b, opts) {
    let o = opts ?? #{}
    let header_a = o.get("a_path") ?? "a"
    let header_b = o.get("b_path") ?? "b"
    let context = o.get("context") ?? 3
    let ops = lines(a, b)

    var out = "--- " + header_a + "\n+++ " + header_b + "\n"
    var hunk = []
    var line_a = 0
    var line_b = 0
    var hunk_a_start = 0
    var hunk_b_start = 0
    var hunk_a_count = 0
    var hunk_b_count = 0
    var pending_context = []
    var trailing_context = 0

    fn flush_hunk() {
        if hunk.is_empty() { return }
        out = out + "@@ -" + str(hunk_a_start) + "," + str(hunk_a_count)
                  + " +" + str(hunk_b_start) + "," + str(hunk_b_count) + " @@\n"
        for h in hunk { out = out + h + "\n" }
        hunk = []
        hunk_a_count = 0
        hunk_b_count = 0
    }

    for op in ops {
        if op.op == "=" {
            line_a = line_a + 1
            line_b = line_b + 1
            if not hunk.is_empty() and trailing_context < context {
                hunk.push(" " + op.line)
                hunk_a_count = hunk_a_count + 1
                hunk_b_count = hunk_b_count + 1
                trailing_context = trailing_context + 1
            } else {
                if not hunk.is_empty() { flush_hunk() }
                pending_context.push(op.line)
                if pending_context.len() > context { pending_context = pending_context.slice(1, pending_context.len()) }
            }
        } else if op.op == "+" {
            line_b = line_b + 1
            if hunk.is_empty() {
                hunk_a_start = line_a + 1 - pending_context.len()
                if hunk_a_start < 1 { hunk_a_start = 1 }
                hunk_b_start = line_b - pending_context.len()
                if hunk_b_start < 1 { hunk_b_start = 1 }
                for ctx in pending_context {
                    hunk.push(" " + ctx)
                    hunk_a_count = hunk_a_count + 1
                    hunk_b_count = hunk_b_count + 1
                }
                pending_context = []
            }
            hunk.push("+" + op.line)
            hunk_b_count = hunk_b_count + 1
            trailing_context = 0
        } else if op.op == "-" {
            line_a = line_a + 1
            if hunk.is_empty() {
                hunk_a_start = line_a - pending_context.len()
                if hunk_a_start < 1 { hunk_a_start = 1 }
                hunk_b_start = line_b + 1 - pending_context.len()
                if hunk_b_start < 1 { hunk_b_start = 1 }
                for ctx in pending_context {
                    hunk.push(" " + ctx)
                    hunk_a_count = hunk_a_count + 1
                    hunk_b_count = hunk_b_count + 1
                }
                pending_context = []
            }
            hunk.push("-" + op.line)
            hunk_a_count = hunk_a_count + 1
            trailing_context = 0
        }
    }
    flush_hunk()
    return out
}
