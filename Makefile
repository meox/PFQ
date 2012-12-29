# (C) 2011-13 Nicola Bonelli <nicola.bonelli@cnit.it>
#

GHCFLAGS= --make -W -O2 -threaded -with-rtsopts="-N"

INSTDIR=/usr/local

LIBS= -lpfq

HC=ghc

HSC=hsc2hs

.PHONY: all clean

all: Network/PFq.hs pfq-read pfq-counters pfq-dispatch

Network/PFq.hs: 
		$(HSC) Network/PFq.hsc 

pfq-read: Network/PFq.hs pfq-read.hs  
		$(HC) $(GHCFLAGS) $(LIBS) pfq-read.hs -o $@

pfq-counters: Network/PFq.hs pfq-counters.hs
		$(HC) $(GHCFLAGS) $(LIBS) pfq-counters.hs -o $@

pfq-dispatch: Network/PFq.hs pfq-dispatch.hs 
		$(HC) $(GHCFLAGS) $(LIBS) pfq-dispatch.hs -o $@

install: all
		@mkdir -p ${INSTDIR}/include/pfq 
		cp pfq_kcompat.h  ${INSTDIR}/include/pfq/
		cp irq-affinity  ${INSTDIR}/bin/
		cp pfq-omatic ${INSTDIR}/bin/
        
clean:
	   @rm -f pfq-read pfq-counters pfq-dispatch
	   @rm -f *.o *.hi Network/*.o Network/*.hi Network/PFq.hs Network/PFq_stub.h
