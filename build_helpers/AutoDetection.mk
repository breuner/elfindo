#
# Auto detection of available system libraries etc.
#

TEST_C_FILE    = $(BUILD_HELPERS_PATH)/AutoDetection.c
TEST_OBJ_FILE  = $(BUILD_HELPERS_PATH)/AutoDetection.obj


clean-buildhelpers:
ifdef BUILD_VERBOSE
	rm -f $(TEST_OBJ_FILE)
else
	@echo "[DELETE] BUILD_HELPERS"
	@rm -f $(TEST_OBJ_FILE)
endif
