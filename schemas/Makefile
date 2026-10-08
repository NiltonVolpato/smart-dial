.PHONY: all clean

TETHER_SCHEMA := ../../../tether-worktree/main/schema
FBS := coprocessor.fbs common.fbs wifi.fbs system.fbs sonos.fbs
CPP_DIR := ../coprocessor/include
RUST_DIR := ../portable/crates/coprocessor/src

CPP_OUT := $(CPP_DIR)/coprocessor_generated.h $(CPP_DIR)/coprocessor_rpc.h
RUST_OUT := $(RUST_DIR)/coprocessor_generated.rs $(RUST_DIR)/coprocessor_rpc.rs
STAMP := .flatc.stamp

all: $(STAMP)

$(STAMP): $(FBS) Makefile
	flatc --cpp --gen-object-api --cpp-std c++17 --scoped-enums --gen-all -I $(TETHER_SCHEMA) -o $(CPP_DIR)/ coprocessor.fbs
	tether-gen coprocessor.fbs -I $(TETHER_SCHEMA) --lang cpp --include coprocessor_generated.h > $(CPP_DIR)/coprocessor_rpc.h
	flatc --rust --gen-object-api --gen-all -I $(TETHER_SCHEMA) -o $(RUST_DIR)/ coprocessor.fbs
	tether-gen coprocessor.fbs -I $(TETHER_SCHEMA) --types crate::coprocessor_generated > $(RUST_DIR)/coprocessor_rpc.rs
	cargo +nightly fmt --manifest-path ../portable/Cargo.toml -- $(RUST_OUT)
	touch $(STAMP)

clean:
	rm -f $(CPP_OUT) $(RUST_OUT) $(STAMP)
