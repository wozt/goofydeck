#!/usr/bin/env ruby
# Send Sonic Pi code to a running Sonic Pi server (headless) via OSC /run-code.
#
# Usage:
#   sonicpi_send_run_code.rb <server_port> <code_file>
#
# Notes:
# - Uses Sonic Pi's own OSC encoder/client from the installed Sonic Pi package.

server_port = (ARGV[0] || "").to_i
code_file = ARGV[1]

if server_port <= 0 || code_file.nil? || code_file.empty?
  warn "Usage: #{File.basename($0)} <server_port> <code_file>"
  exit 2
end

code = File.read(code_file, mode: "rb").force_encoding("utf-8")

lib_dir = "/usr/lib/sonic-pi/app/server/ruby/lib"
$LOAD_PATH.unshift(lib_dir) unless $LOAD_PATH.include?(lib_dir)

require "sonicpi/osc/udp_client"

client = SonicPi::OSC::UDPClient.new("127.0.0.1", server_port)
client.send("/run-code", 0, code)

