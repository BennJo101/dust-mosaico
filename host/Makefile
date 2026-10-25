# Render a few wall states on your computer to check the slicing:  make && ./dust_host out/
# Test the wall's agreement, commands, voice codec and who answers:  make net_test && ./net_test
CFLAGS ?= -O2 -Wall -Wextra
dust_host: host_main.c ../main/dust_render.c ../main/dust_layout.c ../main/dust_warp.c ../main/dust_points.h
	$(CC) $(CFLAGS) -I../main host_main.c ../main/dust_render.c ../main/dust_layout.c ../main/dust_warp.c -lm -o $@
clean:
	rm -f dust_host topo_test net_test
topo_test: topo_test.c ../main/dust_topo.c
	$(CC) $(CFLAGS) -I../main topo_test.c ../main/dust_topo.c -lm -o $@
NET_SRC = ../main/dust_consensus.c ../main/dust_reliable.c ../main/dust_arb.c ../main/dust_adpcm.c
net_test: net_test.c $(NET_SRC) ../main/mosaic_net.h ../bridge/main/link_frame.c
	$(CC) $(CFLAGS) -Istub -I../main -I../bridge/main net_test.c $(NET_SRC) ../bridge/main/link_frame.c -lm -o $@
test: topo_test net_test
	./topo_test && ./net_test
