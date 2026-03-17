(function_declaration body: (_) @function.inside) @function.around
(function_declaration parameters: (parameter_list) @parameter.around)
(parameter) @parameter.inside

(class_declaration body: (_) @class.inside) @class.around
(struct_declaration body: (_) @class.inside) @class.around
(enum_declaration   body: (_) @class.inside) @class.around
(trait_declaration  body: (_) @class.inside) @class.around
(impl_declaration   body: (_) @class.inside) @class.around

(line_comment)  @comment.inside
(block_comment) @comment.inside

((line_comment)+) @comment.around
(block_comment)   @comment.around
