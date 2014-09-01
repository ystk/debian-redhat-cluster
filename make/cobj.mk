%.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(REENT_CFLAGS) -c -o $@ $<

# used by dlm/libdlm
%_lt.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@ $<

# used by rgmanager/src/daemons
%-noccs.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(NOCCS_CFLAGS) -c -o $@ $<
