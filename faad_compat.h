#ifndef FAAD_COMPAT_H
#define FAAD_COMPAT_H

/* The Kindle sysroot keeps FAAD2 in a faad/ subdirectory; distro packages put
 * neaacdec.h at the top of the include path. Include this rather than picking
 * one of the two spellings. */

#if defined(__has_include)
#  if __has_include(<faad/neaacdec.h>)
#    include <faad/neaacdec.h>
#  else
#    include <neaacdec.h>
#  endif
#else
#  include <faad/neaacdec.h>
#endif

#endif /* FAAD_COMPAT_H */
