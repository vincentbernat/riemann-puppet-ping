# riemann-puppet-ping

Actively probe ICMP latency of hosts that report to puppet.
The idea for this tool is that when you rely exclusively on
passive alerts in riemann, you still need a way to make
sure nodes are still up.

## Installation

### Dependencies

build dependencies (ubuntu names):

* libevent-dev
* protobuf-c-compiler
* libprotobuf-c0-dev

run dependencies (ubuntu names):

* libevent
* libprotobuf-c0

### Building

For now, riemann-puppet-ping is a source only distribution, you may build
it on linux by running make and make install.

### Package creation

run `make package` to build a debian package with `fpm`, you can
override the snapshot type version by supplying a VERSION environment
variable

## Configuration

The configuration file expects a simple `key = value` format,
empty lines are ignored, lines starting with a hash are ignored.

The following configuration directives are valid:

* `interval`: interval at which to run the query
* `delay`: delay to add to the interval before marking an event as expired
* `tag`: add this tag to the outbound events. may be called several times
* `reports_dir`: puppet report directory
* `riemann_host`: host the riemann instance lives on
* `riemann_port`: tcp port the riemann instance lives on

## Running

riemann-puppet-ping bundles an upstart script, letting you interact with it using
the service command.

## Caveats

This is a bare-bones release which makes the following assumptions:

* you have a tcp-server running on your riemann instance
* your mysql and riemann servers are reachable through ipv4


