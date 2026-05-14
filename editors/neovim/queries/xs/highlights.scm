; Neovim tree-sitter highlights for XS.
; Uses :h treesitter-highlight-groups names from nvim-treesitter.
; Divergences from canonical noted inline.

; ---- comments ---------------------------------------------------------------

(line_comment)  @comment.line
(block_comment) @comment.block

; ---- literals ---------------------------------------------------------------

(string_literal)  @string
; Neovim: @character for single-char / char literals
(char_literal)    @character

; Neovim uses @number and @number.float
(duration_literal) @number.float
(number_literal)   @number

; Neovim: @boolean, not @constant.builtin for true/false
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
] @keyword.type

[
  "import" "export" "from" "use" "as" "load" "module" "plugin"
] @keyword.import

[
  "if" "elif" "else"
  "match" "when"
  "try" "catch" "finally" "throw"
  "defer"
  "do" "with"
] @keyword.conditional

[
  "while" "for" "in" "loop"
  "break" "continue"
] @keyword.repeat

"return" @keyword.return
"yield"  @keyword.return

[
  "async" "await" "spawn"
  "perform" "handle" "resume" "pause"
] @keyword.coroutine

[
  "pub" "mut" "static" "inline" "unsafe"
] @keyword.modifier

[
  "assert" "panic" "del" "where"
] @keyword

["and" "or" "not" "is"] @keyword.operator

; ---- function / call --------------------------------------------------------

(function_declaration name: (identifier) @function)
(function_signature   name: (identifier) @function)

(call_expression callee: (identifier) @function.call)
(call_expression callee: (field_expression field: (identifier) @function.method.call))

(call_expression callee: (identifier) @function.builtin
  (#match? @function.builtin
    "^(print|println|eprint|eprintln|input|len|type|range|typeof|dbg|pprint|repr|exit|todo|unreachable|copy|clone|assert|assert_eq|panic|str|int|float|push|pop|sorted|reversed|enumerate|sum|min|max|first|last|any|all|map|filter|reduce|flatten|unique)$"))

; ---- variables / parameters -------------------------------------------------

; Neovim: @parameter (legacy), also @variable.parameter in newer versions
(parameter name: (identifier) @variable.parameter)

(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)

((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))

; ---- struct / enum fields ---------------------------------------------------

; Neovim: @field for struct/record field access
(struct_field name: (identifier) @variable.member)
(enum_variant name: (identifier) @constructor)
(field_expression field: (identifier) @variable.member)

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
