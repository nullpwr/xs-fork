#ifndef XS_CONFLICT_H
#define XS_CONFLICT_H
#include "plugins/pipeline.h"

int conflict_check_metas(PluginPipeline *p);
int conflict_check_tokens(PluginPipeline *p);
int conflict_check_node_names(PluginPipeline *p);
int conflict_check_sema_exclusive(PluginPipeline *p);
int conflict_check_pass_cycles(PluginPipeline *p);
int conflict_warn_transform_visibility(PluginPipeline *p);

#endif
