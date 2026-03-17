-- xs-markdown: subset of CommonMark suitable for READMEs and blog posts.
--
-- Supported: ATX headers, paragraphs, code fences, inline code, emphasis,
-- strong, links, images, ordered and unordered lists, hr, blockquotes,
-- inline html passthrough.
-- Not supported: tables, footnotes, definition lists, deeply nested lists.

fn _esc(s) {
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
}

fn _inline(text) {
    var s = text
    -- code spans `...`
    s = _replace_pattern(s, "`", "`",
        |inner| "<code>" + _esc(inner) + "</code>")
    -- images ![alt](url)
    s = _replace_link(s, true)
    -- links [text](url)
    s = _replace_link(s, false)
    -- bold **...**
    s = _replace_pattern(s, "**", "**",
        |inner| "<strong>" + inner + "</strong>")
    -- italic *...*
    s = _replace_pattern(s, "*", "*",
        |inner| "<em>" + inner + "</em>")
    return s
}

fn _replace_pattern(s, open, close, transform) {
    var out = ""
    var i = 0
    let ol = open.len()
    let cl = close.len()
    while i < s.len() {
        if i + ol <= s.len() and s.slice(i, i + ol) == open {
            let end = s.find(close, i + ol)
            if end > 0 {
                out = out + transform(s.slice(i + ol, end))
                i = end + cl
                continue
            }
        }
        out = out + s.slice(i, i + 1)
        i = i + 1
    }
    return out
}

fn _replace_link(s, is_image) {
    let prefix = if is_image { "![" } else { "[" }
    var out = ""
    var i = 0
    while i < s.len() {
        if i + prefix.len() <= s.len() and s.slice(i, i + prefix.len()) == prefix {
            let close_bracket = s.find("]", i + prefix.len())
            if close_bracket > 0 and close_bracket + 1 < s.len()
                and s.slice(close_bracket + 1, close_bracket + 2) == "(" {
                let close_paren = s.find(")", close_bracket + 2)
                if close_paren > 0 {
                    let label = s.slice(i + prefix.len(), close_bracket)
                    let url = s.slice(close_bracket + 2, close_paren)
                    if is_image {
                        out = out + "<img src=\"" + url + "\" alt=\"" + label + "\">"
                    } else {
                        out = out + "<a href=\"" + url + "\">" + label + "</a>"
                    }
                    i = close_paren + 1
                    continue
                }
            }
        }
        out = out + s.slice(i, i + 1)
        i = i + 1
    }
    return out
}

fn render(md) {
    let lines = md.split("\n")
    var out = ""
    var i = 0
    var in_list = false
    var list_tag = ""
    var paragraph = ""

    fn close_paragraph() {
        if paragraph.len() > 0 {
            out = out + "<p>" + _inline(paragraph) + "</p>\n"
            paragraph = ""
        }
    }
    fn close_list() {
        if in_list {
            out = out + "</" + list_tag + ">\n"
            in_list = false
        }
    }

    while i < lines.len() {
        let line = lines[i]
        let trimmed = line.trim()
        if trimmed == "" {
            close_paragraph()
            close_list()
            i = i + 1
            continue
        }
        if trimmed.startswith("```") {
            close_paragraph()
            close_list()
            let lang = trimmed.slice(3, trimmed.len()).trim()
            var code = ""
            i = i + 1
            while i < lines.len() and not lines[i].trim().startswith("```") {
                code = code + _esc(lines[i]) + "\n"
                i = i + 1
            }
            let cls = if lang.len() > 0 { " class=\"language-" + lang + "\"" } else { "" }
            out = out + "<pre><code" + cls + ">" + code + "</code></pre>\n"
            i = i + 1
            continue
        }
        if trimmed.startswith("# ") {
            close_paragraph(); close_list()
            out = out + "<h1>" + _inline(trimmed.slice(2, trimmed.len())) + "</h1>\n"
            i = i + 1; continue
        }
        if trimmed.startswith("## ") {
            close_paragraph(); close_list()
            out = out + "<h2>" + _inline(trimmed.slice(3, trimmed.len())) + "</h2>\n"
            i = i + 1; continue
        }
        if trimmed.startswith("### ") {
            close_paragraph(); close_list()
            out = out + "<h3>" + _inline(trimmed.slice(4, trimmed.len())) + "</h3>\n"
            i = i + 1; continue
        }
        if trimmed == "---" or trimmed == "***" {
            close_paragraph(); close_list()
            out = out + "<hr>\n"
            i = i + 1; continue
        }
        if trimmed.startswith("> ") {
            close_paragraph(); close_list()
            out = out + "<blockquote>" + _inline(trimmed.slice(2, trimmed.len())) + "</blockquote>\n"
            i = i + 1; continue
        }
        if trimmed.startswith("- ") or trimmed.startswith("* ") {
            close_paragraph()
            if not in_list or list_tag != "ul" {
                close_list()
                out = out + "<ul>\n"
                in_list = true; list_tag = "ul"
            }
            out = out + "  <li>" + _inline(trimmed.slice(2, trimmed.len())) + "</li>\n"
            i = i + 1; continue
        }
        if trimmed.len() > 2 and trimmed.slice(0, 1).to_int() > 0
            and trimmed.slice(1, 2) == "." {
            close_paragraph()
            if not in_list or list_tag != "ol" {
                close_list()
                out = out + "<ol>\n"
                in_list = true; list_tag = "ol"
            }
            out = out + "  <li>" + _inline(trimmed.slice(3, trimmed.len()).trim()) + "</li>\n"
            i = i + 1; continue
        }
        if paragraph.len() > 0 { paragraph = paragraph + " " }
        paragraph = paragraph + trimmed
        i = i + 1
    }
    close_paragraph()
    close_list()
    return out
}
