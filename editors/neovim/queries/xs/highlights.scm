; Mirrors editors/tree-sitter-xs/queries/highlights.scm but uses the
; capture names Neovim's :h treesitter-highlight-groups expects.

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

["true" "false"] @boolean
"null" @constant.builtin
["self" "super"] @variable.builtin

(number_literal) @number
(universal_literal) @number
(string_literal) @string
(char_literal) @character
(line_comment)  @comment
(block_comment) @comment

(primitive_type)  @type.builtin
(type_identifier) @type

(function_declaration name: (identifier) @function)
(function_signature  name: (identifier) @function)
(call_expression callee: (identifier) @function.call)
(call_expression callee: (field_expression field: (identifier) @function.method))
(field_expression field: (identifier) @field)
(parameter name: (identifier) @parameter)
(let_declaration   name: (identifier) @variable)
(var_declaration   name: (identifier) @variable)
(const_declaration name: (identifier) @constant)
(struct_field name: (identifier) @field)
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
