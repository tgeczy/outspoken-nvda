/* osp_host.c -- a Macintosh just large enough to run MacinTalk.
 *
 * The 1984 MacinTalk driver (`DRVR 1030`, named `.sp`) is 21,272 bytes of
 * 68000.  We do not emulate a Macintosh; we emulate the handful of things this
 * one driver reaches for.  Everything it touches is documented in
 * docs/sound-model.md and docs/driver-api.md, and both were written by reading
 * the binary rather than by running it, so this host starts from a map rather
 * than from guesses.
 *
 * Design rules carried over from pctalker-nvda and Jayson Smith's EchoTalk,
 * both of which were debugged the hard way:
 *
 *   * Every budget gets a counter and non-zero is a fault, never a silent
 *     truncation.  EchoTalk's first fix failed because the CPU was not given
 *     enough time to finish, and a silent limit is indistinguishable from a
 *     broken program.
 *   * Every unhandled exception is identified by vector, not reported as a
 *     generic stop.  "It stopped" costs an evening; "it took vector 3, address
 *     error, at driver+0x1234" costs a minute.
 *   * Traps we did not implement are counted separately from traps we served.
 *     Returning zero for an unimplemented call is a guess, and guesses that
 *     look like successes are the expensive kind.
 *
 * The engine itself is never distributed with this code.  osp_load_image()
 * takes whatever the user extracted from their own copy.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "m68k.h"

#if defined(_WIN32)
#  define OSP_API __declspec(dllexport)
#else
#  define OSP_API
#endif


#include "osp_host_memory.c"
#include "osp_host_files.c"
#include "osp_host_sane.c"
#include "osp_host_stack.c"
#include "osp_host_resources.c"
#include "osp_host_components.c"
#include "osp_host_sound.c"
#include "osp_host_toolbox.c"
#include "osp_host_runtime.c"
#include "osp_host_api.c"
