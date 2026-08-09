#ifndef FAAD_COMPAT_H
#define FAAD_COMPAT_H

/* The Kindle sysroot keeps FAAD2 in a faad/ subdirectory, while distro packages
 * install neaacdec.h at the top of the include path. Include this instead of
 * picking one of the two spellings. */

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
