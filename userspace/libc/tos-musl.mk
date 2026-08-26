# TOS musl build overlay.
#
# GNU ar accepts response files, but musl's upstream archive rule expands every
# libc object on the command line. That can exceed Windows/MSYS process argument
# limits before ar starts. Keep the source tree pristine and override only this
# archive recipe when building through build_tos_musl.sh.

# The stock rule for this header is
#
#     cp $< $@
#     sed -n -e s/__NR_/SYS_/p < $< >> $@
#
# and under MSYS the appending sed is applied before cp's write becomes
# visible, so the file ends up as the sed output with a fragment of the
# copy pasted over the front. The two collide mid-line and the compile
# dies on a syntax error in a generated header, which is a long way from
# where the problem is. WSL gets it right, but WSL has no cross
# toolchain here, so the build cannot simply move there.
#
# One redirect and one writer, so there is no ordering left to get wrong.
obj/include/bits/syscall.h: $(srcdir)/arch/$(ARCH)/bits/syscall.h.in
	mkdir -p $(dir $@)
	{ cat $< ; sed -n -e s/__NR_/SYS_/p $< ; } > $@

lib/libc.a: $(AOBJS)
	rm -f $@
	$(file >$@.rsp,$(AOBJS))
	$(AR) rc $@ @$@.rsp
	rm -f $@.rsp
	$(RANLIB) $@
