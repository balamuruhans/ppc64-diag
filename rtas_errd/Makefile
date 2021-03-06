#
# Makefile for ppc64-diag/rtas_errd
#

include ../rules.mk

CMDS = rtas_errd convert_dt_node_props extract_platdump
SCRIPTS = rc.powerfail

COMMON_UTILS_OBJ = $(COMMON_DIR)/utils.o

RTAS_ERRD_OBJS = rtas_errd.o epow.o dump.o guard.o eeh.o update.o \
		 files.o config.o diag_support.o ela.o v6ela.o servicelog.o \
		 signal.o prrn.o hotplug.o

RTAS_ERRD_LIBS = -lrtas -lrtasevent -lservicelog

DT_NODE_OBJS = convert_dt_node_props.o

EXTRACT_PLATDUMP_OBJS = extract_platdump.o config.o
EXTRACT_PLATDUMP_LIBS = -lrtas

DOXYGEN_CFG = doxygen.config

all: $(CMDS) $(SCRIPTS)

rtas_errd: $(RTAS_ERRD_OBJS) $(COMMON_UTILS_OBJ)
	@echo "LD $(WORK_DIR)/$@"
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(RTAS_ERRD_LIBS)

convert_dt_node_props: $(DT_NODE_OBJS)
	@echo "LD $(WORK_DIR)/$@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

extract_platdump: $(EXTRACT_PLATDUMP_OBJS)
	@echo "LD $(WORK_DIR)/$@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(EXTRACT_PLATDUMP_LIBS)

install: all
	@$(call install_sbin,$(CMDS),$(DESTDIR))
	@$(call install_files,$(SCRIPTS),744,$(DESTDIR)/etc)

uninstall:
	@$(call uninstall_sbin,$(CMDS),$(DESTDIR))
	@$(call uninstall_files,$(SCRIPTS),$(DESTDIR)/etc)

doc: $(DOXYGEN_CFG)
	@echo "Creating doxygen docs..."
	@$(DOXYGEN) $(DOXYGEN_CFG)

clean:
	@echo "Cleaning up $(WORK_DIR) files..."
	@rm -rf $(CMDS) $(RTAS_ERRD_OBJS) $(DT_NODE_OBJS) \
		$(EXTRACT_PLATDUMP_OBJS) doc
