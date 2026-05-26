define module_template
$(1)_SRC_DIR := $$(SRC_DIR)/$(2)
$(1)_BUILD_DIR := $$(BUILD_DIR)/$(2)
$(1)_OBJ := $$($(1)_BUILD_DIR).o
$(1)_DEPS := $$($(1)_BUILD_DIR).d
$(1)_C_SRCS := $$(filter %.c,$$($(1)_SRCS))
$(1)_S_SRCS := $$(filter %.S,$$($(1)_SRCS))
$(1)_C_OBJS := $$(addprefix $$($(1)_BUILD_DIR)/,$$($(1)_C_SRCS:.c=.c.o))
$(1)_S_OBJS := $$(addprefix $$($(1)_BUILD_DIR)/,$$($(1)_S_SRCS:.S=.S.o))
$(1)_OBJS := $$($(1)_C_OBJS) $$($(1)_S_OBJS)
$(1)_SUBMODULE_OBJS := $$(foreach m,$$($(1)_SUBMODULES),$$($(1)_BUILD_DIR)/$$(m).o)

$$($(1)_OBJ): $$($(1)_OBJS) $$($(1)_SUBMODULE_OBJS) | $$($(1)_BUILD_DIR)
	$$(call log,LD,$$@)
	@$$(LD) -r -o $$@ --whole-archive $$($(1)_OBJS) $$($(1)_SUBMODULE_OBJS) --no-whole-archive

$$($(1)_BUILD_DIR):
	@mkdir -p $$@

$$($(1)_BUILD_DIR)/%.c.o: $$($(1)_SRC_DIR)/%.c | $$($(1)_BUILD_DIR)
	$$(call log,CC,$$<)
	@$$(CC) $$(C_FLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

$$($(1)_BUILD_DIR)/%.S.o: $$($(1)_SRC_DIR)/%.S | $$($(1)_BUILD_DIR)
	$$(call log,AS,$$<)
	@$$(CC) $$(AS_FLAGS) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

-include $$(patsubst %.o,%.d,$$($(1)_OBJS))

endef

MODULE_NAME := $(subst /,_,$(MODULE_PATH))
$(MODULE_NAME)_SRCS := $(SRCS)
$(MODULE_NAME)_SUBMODULES := $(SUBMODULES)

$(eval $(call module_template,$(MODULE_NAME),$(MODULE_PATH)))

$(if $(strip $($(MODULE_NAME)_SUBMODULES)),\
  $(eval _SAVED_MODULE_PATH := $(MODULE_PATH))\
  $(foreach m,$($(MODULE_NAME)_SUBMODULES),\
    $(eval include $(SRC_DIR)/$(_SAVED_MODULE_PATH)/$(m)/Makefile) \
  ) \
)
