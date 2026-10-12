# Host-only capture tools (x86-64). Output goes to refs/cap/ (untracked).
CFLAGS = -O2 -fPIC -Wall -Wno-unused-parameter
all: libpmcap_state.so pmcap-input.so pmcap-video.so
libpmcap_state.so: pmcap_state.c ; $(CC) $(CFLAGS) -shared -o $@ $<
pmcap-input.so: pmcap_input.c libpmcap_state.so ; $(CC) $(CFLAGS) -shared -o $@ $< -L. -lpmcap_state -Wl,-rpath,'$$ORIGIN' -ldl
pmcap-video.so: pmcap_video.c libpmcap_state.so ; $(CC) $(CFLAGS) -shared -o $@ $< -L. -lpmcap_state -Wl,-rpath,'$$ORIGIN'
clean: ; rm -f *.so
