// SPDX-License-Identifier: MIT
// Enable the pinned Mesa validator for untimed prototype checks, even though
// the existing compiler libraries are release builds. Upstream notices remain
// in the included source file.
#undef NDEBUG
#include "compiler/nir/nir_validate.c"
uint32_t nir_debug=0;
void prototype_validate(nir_shader *);
void prototype_validate(nir_shader *shader){
    const char *prefix=getenv("AIR_NIR_VALIDATE_DUMP");static unsigned sequence;
    if(prefix){char path[2048];snprintf(path,sizeof(path),"%s-%u.nir",prefix,sequence++);FILE *file=fopen(path,"w");if(file){nir_print_shader(shader,file);fclose(file);}}
    nir_validate_shader(shader,"AIR direct prototype");
}
