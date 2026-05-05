; Helix uses standard tree-sitter capture names with @keyword,
; @string, etc. — same set as Neovim, modulo a couple naming choices.

[
  "fn" "fn*" "let" "var" "const"
  "struct" "enum" "trait" "impl" "class" "type" "effect" "tag"
  "import" "export" "from" "use" "module" "as" "plugin"
  "if" "elif" "else" "match" "when" "while" "for" "in" "loop"
  "return" "break" "continue" "yield"
  "try" "catch" "finally" "throw" "defer"
  "async" "await" "spawn" "nursery" "actor"
  "perform" "handle" "resume"
  "pub" "mut" "static" "inline" "unsafe" "where"
] @keyword

["and" "or" "not" "is"] @keyword.operator

["true" "false"] @constant.builtin.boolean
"null" @constant.builtin
["self" "super"] @variable.builtin

(number_literal) @constant.numeric
(universal_literal) @constant.numeric
(string_literal) @string
(char_literal) @constant.character
(line_comment)  @comment
(block_comment) @comment

(decorator) @attribute
(decorator name: (identifier) @attribute)

(primitive_type)  @type.builtin
(type_identifier) @type

(function_declaration name: (identifier) @function)
(function_signature  name: (identifier) @function)
(call_expression callee: (identifier) @function)
(call_expression callee: (field_expression field: (identifier) @function.method))
(field_expression field: (identifier) @variable.other.member)
(parameter name: (identifier) @variable.parameter)
(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)
(struct_field name: (identifier) @variable.other.member)
(enum_variant name: (identifier) @constructor)

[
  "+" "-" "*" "/" "%" "**"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!" "~"
  "&" "|" "^" "<<" ">>"
  "=" "+=" "-=" "*=" "/=" "%="
  ".." "..="
  "??" "|>" "->"
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "."] @punctuation.delimiter
