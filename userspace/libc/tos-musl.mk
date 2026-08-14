# TOS musl build overlay.
#
# GNU ar accepts response files, but musl's upstream archive rule expands every
# libc object on the command line. That can exceed Windows/MSYS process argument
# limits before ar starts. Keep the source tree pristine and override only this
# archive recipe when building through build_tos_musl.sh.

lib/libc.a: $(AOBJS)
	rm -f $@
	$(file >$@.rsp,$(AOBJS))
	$(AR) rc $@ @$@.rsp
	rm -f $@.rsp
	$(RANLIB) $@
