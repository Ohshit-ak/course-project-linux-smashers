# Root Makefile for Docs++ Distributed File System

.PHONY: all clean client naming_server storage_server common

all: common client naming_server storage_server

common:
	@echo "Building common utilities..."
	@cd common && $(MAKE)

client: common
	@echo "Building client..."
	@cd client && $(MAKE)

naming_server: common
	@echo "Building naming server..."
	@cd naming_server && $(MAKE)

storage_server: common
	@echo "Building storage server..."
	@cd storage_server && $(MAKE)

clean:
	@echo "Cleaning all build files..."
	@cd common && $(MAKE) clean
	@cd client && $(MAKE) clean
	@cd naming_server && $(MAKE) clean
	@cd storage_server && $(MAKE) clean
	@rm -f *.log

run_ns:
	@echo "Starting Naming Server..."
	@./naming_server/naming_server

run_ns_modular:
	@echo "Starting Naming Server (Modular)..."
	@./naming_server/naming_server_modular

run_ss1:
	@echo "Starting Storage Server 1..."
	@./storage_server/storage_server SS1 127.0.0.1 8080 8081

run_ss1_modular:
	@echo "Starting Storage Server 1 (Modular)..."
	@./storage_server/storage_server_modular SS1 127.0.0.1 8080 8081

run_ss2:
	@echo "Starting Storage Server 2..."
	@./storage_server/storage_server SS2 127.0.0.1 8080 8082

run_ss2_modular:
	@echo "Starting Storage Server 2 (Modular)..."
	@./storage_server/storage_server_modular SS2 127.0.0.1 8080 8082

run_client:
	@echo "Starting Client..."
	@./client/client

run_client_modular:
	@echo "Starting Client (Modular)..."
	@./client/client_modular

help:
	@echo "Docs++ Distributed File System - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all            - Build all components (default)"
	@echo "  clean          - Remove all build files"
	@echo "  client         - Build only client"
	@echo "  naming_server  - Build only naming server"
	@echo "  storage_server - Build only storage server"
	@echo ""
	@echo "Running components (in separate terminals):"
	@echo "  make run_ns            - Start Naming Server (original)"
	@echo "  make run_ns_modular    - Start Naming Server (modular)"
	@echo "  make run_ss1           - Start Storage Server 1 (original)"
	@echo "  make run_ss1_modular   - Start Storage Server 1 (modular)"
	@echo "  make run_ss2           - Start Storage Server 2 (original)"
	@echo "  make run_ss2_modular   - Start Storage Server 2 (modular)"
	@echo "  make run_client        - Start Client (original)"
	@echo "  make run_client_modular- Start Client (modular)"
