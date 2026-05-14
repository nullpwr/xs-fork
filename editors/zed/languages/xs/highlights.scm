; Zed tree-sitter highlights for XS.
; Capture names follow Zed's theme token set (Tree-sitter standard with
; a few Zed-specific preferences noted inline).

; ---- comments ---------------------------------------------------------------

(line_comment)  @comment.line
(block_comment) @comment.block

; ---- literals ---------------------------------------------------------------

(string_literal)  @string
; Zed maps char-like things to @string.special
(char_literal)    @string.special

(duration_literal) @number
(number_literal)   @number

["true" "false"] @boolean
"null" @constant.builtin

; ---- self / super -----------------------------------------------------------

["self" "super"] @variable.builtin

; ---- decorators -------------------------------------------------------------

(decorator) @attribute
(decorator name: (identifier) @attribute)

; ---- types ------------------------------------------------------------------

(primitive_type)  @type.builtin
(type_identifier) @type
(generic_type (type_identifier) @type)

; ---- keywords ---------------------------------------------------------------

"fn"      @keyword.function
"fn*"     @keyword.function

[
  "let" "var" "const"
  "struct" "enum" "trait" "impl" "class" "type"
  "effect" "tag" "bind" "actor" "nursery" "macro"
] @keyword

[
  "import" "export" "from" "use" "as" "load" "module" "plugin"
] @keyword

[
  "if" "elif" "else"
  "match" "when"
  "try" "catch" "finally" "throw"
  "defer"
  "do" "with"
] @keyword

[
  "while" "for" "in" "loop"
  "break" "continue"
] @keyword

"return" @keyword.return
"yield"  @keyword.return

[
  "async" "await" "spawn"
  "perform" "handle" "resume" "pause"
] @keyword

[
  "pub" "mut" "static" "inline" "unsafe"
] @keyword

[
  "assert" "panic" "del" "where"
] @keyword

["and" "or" "not" "is"] @operator

; ---- function / call --------------------------------------------------------

(function_declaration name: (identifier) @function)
(function_signature   name: (identifier) @function)

(call_expression callee: (identifier) @function)
(call_expression callee: (field_expression field: (identifier) @function.method))

(call_expression callee: (identifier) @function.builtin
  (#match? @function.builtin
    "^(print|println|eprint|eprintln|input|len|type|range|typeof|dbg|pprint|repr|exit|todo|unreachable|copy|clone|assert|assert_eq|panic|str|int|float|push|pop|sorted|reversed|enumerate|sum|min|max|first|last|any|all|map|filter|reduce|flatten|unique)$"))

; ---- variables / parameters -------------------------------------------------

(parameter name: (identifier) @variable.parameter)

(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)

((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))

; ---- struct / enum fields ---------------------------------------------------

(struct_field name: (identifier) @property)
(enum_variant name: (identifier) @constructor)
; Zed prefers @property for field access
(field_expression field: (identifier) @property)

; ---- operators --------------------------------------------------------------

[
  "+" "-" "*" "/" "%" "**"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!" "~"
  "&" "|" "^" "<<" ">>"
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  ".." "..="
  "??" "|>" "->" "=>"
  "++"
] @operator

; ---- punctuation ------------------------------------------------------------

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "."]          @punctuation.delimiter
