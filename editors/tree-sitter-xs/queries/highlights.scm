; keywords
[
  "fn" "fn*" "let" "var" "const"
  "struct" "enum" "trait" "impl" "class" "type" "effect" "tag"
  "import" "use" "export" "from" "as" "plugin"
  "if" "elif" "else" "match" "when" "while" "for" "in" "loop"
  "return" "break" "continue" "yield"
  "try" "catch" "finally" "throw" "defer"
  "async" "await" "spawn" "nursery" "actor"
  "perform" "handle" "resume"
  "pub" "mut" "static" "inline" "unsafe"
  "where"
] @keyword

["and" "or" "not" "is"] @keyword.operator

["true" "false" "null"] @constant.builtin
["self" "super"] @variable.builtin

; literals
(number_literal) @number
(string_literal) @string
(char_literal) @string
(boolean_literal) @constant.builtin
(null_literal) @constant.builtin
; durations get their own capture so themes can paint 5s / 100ms
; differently from a bare number - they are a real first-class type.
(duration_literal) @constant.numeric.duration

; comments
(line_comment)  @comment
(block_comment) @comment

; decorators
(decorator) @attribute
(decorator name: (identifier) @attribute)

; types
(primitive_type)  @type.builtin
(type_identifier) @type
(generic_type (type_identifier) @type)

; functions
(function_declaration name: (identifier) @function)
(function_signature  name: (identifier) @function)
(call_expression callee: (identifier) @function.call)
(call_expression callee: (field_expression field: (identifier) @function.method))
(field_expression field: (identifier) @property)

; parameters
(parameter name: (identifier) @variable.parameter)

; bindings
(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)

; struct/enum fields
(struct_field name: (identifier) @property)
(enum_variant name: (identifier) @constructor)

; operators
[
  "+" "-" "*" "/" "%" "**"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!" "~"
  "&" "|" "^" "<<" ">>"
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  ".." "..="
  "??" "|>" "->"
] @operator

; punctuation
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "."]          @punctuation.delimiter
