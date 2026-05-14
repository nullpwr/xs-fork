; Helix tree-sitter highlights for XS.
; Capture names follow Helix's theme token set.
; Divergences from canonical noted inline.

; ---- comments ---------------------------------------------------------------

(line_comment)  @comment.line
(block_comment) @comment.block

; ---- literals ---------------------------------------------------------------

(string_literal)  @string
; Helix: char / special strings -> @constant.character
(char_literal)    @constant.character

(duration_literal) @constant.numeric.float
(number_literal)   @constant.numeric

["true" "false"] @constant.builtin.boolean
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
] @keyword.storage.type

[
  "import" "export" "from" "use" "as" "load" "module" "plugin"
] @keyword.control.import

[
  "if" "elif" "else"
  "match" "when"
  "try" "catch" "finally" "throw"
  "defer"
  "do" "with"
] @keyword.control

[
  "while" "for" "in" "loop"
  "break" "continue"
] @keyword.control.repeat

"return" @keyword.control.return
"yield"  @keyword.control.return

[
  "async" "await" "spawn"
  "perform" "handle" "resume" "pause"
] @keyword.control

[
  "pub" "mut" "static" "inline" "unsafe"
] @keyword.storage.modifier

[
  "assert" "panic" "del" "where"
] @keyword

["and" "or" "not" "is"] @keyword.operator

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

(struct_field name: (identifier) @variable.other.member)
(enum_variant name: (identifier) @constructor)
; Helix uses @variable.other.member for field access
(field_expression field: (identifier) @variable.other.member)

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
