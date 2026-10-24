#!/usr/bin/env python3
"""
MIDI Auto-Connect for M8C

Automatically connects all MIDI devices to each other using ALSA.
Useful for routing external MIDI controllers to M8.

Based on midi_connect.py from rg351_m8c by Stephen Brown (Little Fish Solutions LTD)
"""

import sys
import logging
import subprocess
import re
import argparse

log = logging.getLogger(__name__)
VERSION = '1.0.0'


def run_aconnect(args: list) -> subprocess.CompletedProcess:
    """Run aconnect with given arguments."""
    return subprocess.run(
        ['aconnect'] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )


def parse_aconnect_output(output: str) -> tuple[dict, list]:
    """
    Parse aconnect -i -l output.
    Returns (clients dict, existing_connections list)
    """
    clients = {}
    existing_connections = []

    # Regular expressions
    client_regex = re.compile(r'^client ([0-9]+): \'([^\']*)\'.*$')
    port_regex = re.compile(r'^\s*([0-9]+) \'([^\']*)\'.*$')
    connecting_to_regex = re.compile(r'^\s*Connecting To: (.*)$')
    connecting_from_regex = re.compile(r'^\s*Connected From:.*$')

    client_number = None
    client_name = None
    port_number = None
    skip_client = False

    # Clients to skip (system/virtual)
    skip_clients = ['System', 'Midi Through']

    for line in output.split('\n'):
        if not line.strip():
            continue

        # Match client line
        client_match = client_regex.match(line)
        if client_match:
            client_number = client_match.group(1)
            client_name = client_match.group(2)
            log.debug(f'Found client {client_number} "{client_name}"')

            skip_client = client_name in skip_clients
            if skip_client:
                log.debug(f' - Skipping client "{client_name}"')
            continue

        # Match port line
        port_match = port_regex.match(line)
        if port_match:
            if client_number is None:
                raise Exception(f'Found port without client: {line}')

            if skip_client:
                continue

            port_number = port_match.group(1)
            port_name = port_match.group(2)
            log.debug(f' - Found port {port_number} "{port_name}"')

            if client_number not in clients:
                clients[client_number] = []
            clients[client_number].append(port_number)
            continue

        # Match connecting to line
        connecting_to_match = connecting_to_regex.match(line)
        if connecting_to_match:
            connecting_from = f'{client_number}:{port_number}'
            connecting_to_str = connecting_to_match.group(1)
            connecting_to_list = [s.strip() for s in connecting_to_str.split(',')]
            for connecting_to in connecting_to_list:
                log.debug(f'Existing connection {connecting_from} -> {connecting_to}')
                existing_connections.append((connecting_from, connecting_to))
            continue

        # Match connected from line (ignore)
        if connecting_from_regex.match(line):
            continue

    return clients, existing_connections


def list_midi_devices():
    """List all MIDI devices."""
    result = run_aconnect(['-i', '-l'])
    print(result.stdout.decode())


def connect_all(dry_run: bool = False):
    """Connect all MIDI devices to each other."""
    result = run_aconnect(['-i', '-l'])
    clients, existing_connections = parse_aconnect_output(result.stdout.decode())

    if not clients:
        log.info('No MIDI devices found.')
        return

    log.info(f'Found {len(clients)} MIDI client(s)')

    connections_made = 0

    for from_client, from_ports in clients.items():
        for to_client, to_ports in clients.items():
            if from_client == to_client:
                continue

            for from_port in from_ports:
                for to_port in to_ports:
                    from_str = f'{from_client}:{from_port}'
                    to_str = f'{to_client}:{to_port}'

                    if (from_str, to_str) in existing_connections:
                        log.info(f'{from_str} already connected to {to_str} - skipping')
                        continue

                    log.info(f'Connecting {from_str} -> {to_str}')

                    if not dry_run:
                        map_result = run_aconnect([from_str, to_str])
                        output = map_result.stdout.decode()
                        if output:
                            log.warning(output)

                    connections_made += 1

    log.info(f'Made {connections_made} new connection(s)')


def disconnect_all():
    """Disconnect all MIDI connections."""
    result = run_aconnect(['-x'])
    if result.returncode == 0:
        log.info('All MIDI connections disconnected')
    else:
        log.error(f'Failed to disconnect: {result.stdout.decode()}')


def main():
    parser = argparse.ArgumentParser(
        description='MIDI Auto-Connect for M8C - Connect all MIDI devices to each other'
    )
    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='Enable verbose output'
    )
    parser.add_argument(
        '-l', '--list',
        action='store_true',
        help='List MIDI devices and exit'
    )
    parser.add_argument(
        '-d', '--disconnect',
        action='store_true',
        help='Disconnect all MIDI connections'
    )
    parser.add_argument(
        '-n', '--dry-run',
        action='store_true',
        help='Show what would be done without making connections'
    )
    parser.add_argument(
        '--version',
        action='version',
        version=f'midi_connect v{VERSION}'
    )

    args = parser.parse_args()

    # Configure logging
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO, format='%(message)s')

    log.info(f'MIDI Connect v{VERSION}')

    if args.list:
        list_midi_devices()
        return

    if args.disconnect:
        disconnect_all()
        return

    connect_all(dry_run=args.dry_run)


if __name__ == '__main__':
    if sys.version_info.major < 3:
        sys.exit('This script requires Python 3')
    main()
