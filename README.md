# riemann-puppet-ping

Actively probe ICMP latency of hosts that report to puppet.
The idea for this tool is that when you rely exclusively on
passive alerts in riemann, you still need a way to make
sure nodes are still up.

## Installation

### Package creation

run `make package` to build a debian package with `fpm`, you can
override the snapshot type version by supplying a VERSION environment
variable

## Configuration

The configuration file expects a simple `key = value` format,
empty lines are ignored, lines starting with a hash are ignored.

The following configuration directives are valid:

* `interval`: interval at which to run the query
* `tags`: add this tag to the outbound events. may be called several times
* `reports_dir`: puppet report directory
* `riemann_host`: host the riemann instance lives on
* `riemann_port`: tcp port the riemann instance lives on
* `shortnames`: truncate hostnames at the first dot

## Running

riemann-puppet-ping bundles an upstart script, letting you interact with it using
the service command.
