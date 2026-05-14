; ---- comments ---------------------------------------------------------------

(line_comment)  @comment.line
(block_comment) @comment.block

; ---- literals ---------------------------------------------------------------

(string_literal)  @string
(char_literal)    @string.special

; duration_literal: 30s, 100ms, 2m30s
(duration_literal) @number

(number_literal) @number

(boolean_literal) @boolean
(null_literal)    @constant.builtin

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

; ---- keywords - split by semantic role -------------------------------------

; declaration: fn, fn*, let, var, const, struct, enum, trait, impl, class,
;              type, effect, tag, bind, actor, nursery, macro
"fn"      @keyword.function
"fn*"     @keyword.function

[
  "let" "var" "const"
  "struct" "enum" "trait" "impl" "class" "type"
  "effect" "tag" "bind" "actor" "nursery" "macro"
] @keyword.type

; import / module
[
  "import" "export" "from" "use" "as" "load" "module" "plugin"
] @keyword.import

; control flow
[
  "if" "elif" "else"
  "match" "when"
  "try" "catch" "finally" "throw"
  "defer"
  "do" "with"
] @keyword.control

; loops
[
  "while" "for" "in" "loop"
  "break" "continue"
] @keyword.repeat

; return / yield
"return" @keyword.return
"yield"  @keyword.return

; async / concurrency / effects
[
  "async" "await" "spawn"
  "perform" "handle" "resume" "pause"
] @keyword.coroutine

; modifiers
[
  "pub" "mut" "static" "inline" "unsafe"
] @keyword.modifier

; other
[
  "assert" "panic" "del" "where"
] @keyword

; logical word-operators
["and" "or" "not" "is"] @keyword.operator

; ---- function / call --------------------------------------------------------

(function_declaration name: (identifier) @function)
(function_signature   name: (identifier) @function)

(call_expression callee: (identifier) @function.call)
(call_expression callee: (field_expression field: (identifier) @function.method.call))

; built-in functions
(call_expression callee: (identifier) @function.builtin
  (#match? @function.builtin
    "^(print|println|eprint|eprintln|input|len|type|range|typeof|dbg|pprint|repr|exit|todo|unreachable|copy|clone|assert|assert_eq|panic|str|int|float|push|pop|sorted|reversed|enumerate|sum|min|max|first|last|any|all|map|filter|reduce|flatten|unique)$"))

; ---- variables / parameters -------------------------------------------------

(parameter name: (identifier) @variable.parameter)

(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)

; UPPER_SNAKE_CASE identifiers as constants
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))

; ---- struct / enum fields ---------------------------------------------------

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
