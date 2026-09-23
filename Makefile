SUBDIRS := src

TARGETS := all clean rebuild

$(TARGETS): $(SUBDIRS)

.PHONY: all $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

.PHONY: l2
l2:
	$(MAKE) -C l2 apps
