#!/usr/bin/env pmpython
#
# Copyright (C) 2015-2017 Marko Myllynen <myllynen@redhat.com>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# [write_es] Copyright (C) 2014-2015 Red Hat, based on pcp2es by Frank Ch. Eigler

# pylint: disable=superfluous-parens
# pylint: disable=invalid-name, line-too-long, no-self-use
# pylint: disable=too-many-boolean-expressions, too-many-statements
# pylint: disable=too-many-instance-attributes, too-many-locals
# pylint: disable=too-many-branches, too-many-nested-blocks
# pylint: disable=bare-except, broad-except

""" PCP to Elasticsearch Bridge """

# Common imports
from collections import OrderedDict
import errno
import time
import sys

# Our imports
from elasticsearch import Elasticsearch, ElasticsearchException

# PCP Python PMAPI
from pcp import pmapi, pmconfig
from cpmapi import PM_CONTEXT_ARCHIVE, PM_ERR_EOL, PM_IN_NULL, PM_DEBUG_APPL1
from cpmapi import PM_TIME_MSEC

if sys.version_info[0] >= 3:
    long = int # pylint: disable=redefined-builtin

# Default config
DEFAULT_CONFIG = ["./pcp2elasticsearch.conf", "$HOME/.pcp2elasticsearch.conf", "$HOME/.pcp/pcp2elasticsearch.conf", "$PCP_SYSCONF_DIR/pcp2elasticsearch.conf"]

# Defaults
CONFVER = 1
ES_INDEX = "pcp"
ES_SERVER = "http://localhost:9200/"

class pcp2elasticsearch(object):
    """ PCP to Elasticsearch """
    def __init__(self):
        """ Construct object, prepare for command line handling """
        self.context = None
        self.daemonize = 0
        self.pmconfig = pmconfig.pmConfig(self)
        self.opts = self.options()

        # Configuration directives
        self.keys = ('source', 'output', 'derived', 'header', 'globals',
                     'samples', 'interval', 'type', 'precision', 'daemonize',
                     'es_server', 'es_index', 'es_hostid',
                     'count_scale', 'space_scale', 'time_scale', 'version',
                     'speclocal', 'instances', 'ignore_incompat', 'omit_flat')

        # The order of preference for options (as present):
        # 1 - command line options
        # 2 - options from configuration file(s)
        # 3 - built-in defaults defined below
        self.check = 0
        self.version = CONFVER
        self.source = "local:"
        self.output = None # For pmrep conf file compat only
        self.speclocal = None
        self.derived = None
        self.header = 1
        self.globals = 1
        self.samples = None # forever
        self.interval = pmapi.timeval(60)      # 60 sec
        self.opts.pmSetOptionInterval(str(60)) # 60 sec
        self.delay = 0
        self.type = 0
        self.ignore_incompat = 0
        self.instances = []
        self.omit_flat = 0
        self.precision = 3 # .3f
        self.timefmt = "%H:%M:%S" # For compat only
        self.interpol = 0
        self.count_scale = None
        self.space_scale = None
        self.time_scale = None

        self.es_server = ES_SERVER
        self.es_index = ES_INDEX
        self.es_hostid = None

        # Internal
        self.runtime = -1

        # Performance metrics store
        # key - metric name
        # values - 0:label, 1:instance(s), 2:unit/scale, 3:type, 4:width, 5:pmfg item
        self.metrics = OrderedDict()
        self.pmfg = None
        self.pmfg_ts = None

        # Read configuration and prepare to connect
        self.config = self.pmconfig.set_config_file(DEFAULT_CONFIG)
        self.pmconfig.read_options()
        self.pmconfig.read_cmd_line()
        self.pmconfig.prepare_metrics()

    def options(self):
        """ Setup default command line argument option handling """
        opts = pmapi.pmOptions()
        opts.pmSetOptionCallback(self.option)
        opts.pmSetOverrideCallback(self.option_override)
        opts.pmSetShortOptions("a:h:LK:c:Ce:D:V?HGA:S:T:O:s:t:rIi:vP:q:b:y:g:x:X:")
        opts.pmSetShortUsage("[option...] metricspec [...]")

        opts.pmSetLongOptionHeader("General options")
        opts.pmSetLongOptionArchive()      # -a/--archive
        opts.pmSetLongOptionArchiveFolio() # --archive-folio
        opts.pmSetLongOptionContainer()    # --container
        opts.pmSetLongOptionHost()         # -h/--host
        opts.pmSetLongOptionLocalPMDA()    # -L/--local-PMDA
        opts.pmSetLongOptionSpecLocal()    # -K/--spec-local
        opts.pmSetLongOption("config", 1, "c", "FILE", "config file path")
        opts.pmSetLongOption("check", 0, "C", "", "check config and metrics and exit")
        opts.pmSetLongOption("derived", 1, "e", "FILE|DFNT", "derived metrics definitions")
        self.daemonize = opts.pmSetLongOption("daemonize", 0, "", "", "daemonize on startup") # > 1
        opts.pmSetLongOptionDebug()        # -D/--debug
        opts.pmSetLongOptionVersion()      # -V/--version
        opts.pmSetLongOptionHelp()         # -?/--help

        opts.pmSetLongOptionHeader("Reporting options")
        opts.pmSetLongOption("no-header", 0, "H", "", "omit headers")
        opts.pmSetLongOption("no-globals", 0, "G", "", "omit global metrics")
        opts.pmSetLongOptionAlign()        # -A/--align
        opts.pmSetLongOptionStart()        # -S/--start
        opts.pmSetLongOptionFinish()       # -T/--finish
        opts.pmSetLongOptionOrigin()       # -O/--origin
        opts.pmSetLongOptionSamples()      # -s/--samples
        opts.pmSetLongOptionInterval()     # -t/--interval
        opts.pmSetLongOption("raw", 0, "r", "", "output raw counter values (no rate conversion)")
        opts.pmSetLongOption("ignore-incompat", 0, "I", "", "ignore incompatible instances (default: abort)")
        opts.pmSetLongOption("instances", 1, "i", "STR", "instances to report (default: all current)")
        opts.pmSetLongOption("omit-flat", 0, "v", "", "omit single-valued metrics with -i (default: include)")
        opts.pmSetLongOption("precision", 1, "P", "N", "N digits after the decimal separator (default: 3)")
        opts.pmSetLongOption("count-scale", 1, "q", "SCALE", "default count unit")
        opts.pmSetLongOption("space-scale", 1, "b", "SCALE", "default space unit")
        opts.pmSetLongOption("time-scale", 1, "y", "SCALE", "default time unit")

        opts.pmSetLongOption("es-host", 1, "g", "SERVER", "elasticsearch server (default: " + ES_SERVER + ")")
        opts.pmSetLongOption("es-index", 1, "x", "INDEX", "elasticsearch index for metric names (default: " + ES_INDEX + ")")
        opts.pmSetLongOption("es-hostid", 1, "X", "HOSTID", "elasticsearch host-id for measurements")

        return opts

    def option_override(self, opt):
        """ Override standard PCP options """
        if opt == 'H' or opt == 'K' or opt == 'g':
            return 1
        return 0

    def option(self, opt, optarg, index):
        """ Perform setup for an individual command line option """
        if index == self.daemonize and opt == '':
            self.daemonize = 1
            return
        if opt == 'K':
            if not self.speclocal or not self.speclocal.startswith("K:"):
                self.speclocal = "K:" + optarg
            else:
                self.speclocal = self.speclocal + "|" + optarg
        elif opt == 'c':
            self.config = optarg
        elif opt == 'C':
            self.check = 1
        elif opt == 'e':
            self.derived = optarg
        elif opt == 'H':
            self.header = 0
        elif opt == 'G':
            self.globals = 0
        elif opt == 'r':
            self.type = 1
        elif opt == 'I':
            self.ignore_incompat = 1
        elif opt == 'i':
            self.instances = self.instances + self.pmconfig.parse_instances(optarg)
        elif opt == 'v':
            self.omit_flat = 1
        elif opt == 'P':
            try:
                self.precision = int(optarg)
            except:
                sys.stderr.write("Error while parsing options: Integer expected.\n")
                sys.exit(1)
        elif opt == 'q':
            self.count_scale = optarg
        elif opt == 'b':
            self.space_scale = optarg
        elif opt == 'y':
            self.time_scale = optarg
        elif opt == 'g':
            self.es_server = optarg
        elif opt == 'x':
            self.es_index = optarg
        elif opt == 'X':
            self.es_hostid = optarg
        else:
            raise pmapi.pmUsageErr()

    def connect(self):
        """ Establish a PMAPI context """
        context, self.source = pmapi.pmContext.set_connect_options(self.opts, self.source, self.speclocal)

        self.pmfg = pmapi.fetchgroup(context, self.source)
        self.pmfg_ts = self.pmfg.extend_timestamp()
        self.context = self.pmfg.get_context()

        if pmapi.c_api.pmSetContextOptions(self.context.ctx, self.opts.mode, self.opts.delta):
            raise pmapi.pmUsageErr()

        self.pmconfig.validate_metrics()

    def validate_config(self):
        """ Validate configuration options """
        if self.version != CONFVER:
            sys.stderr.write("Incompatible configuration file version (read v%s, need v%d).\n" % (self.version, CONFVER))
            sys.exit(1)

        if self.es_hostid is None:
            self.es_hostid = self.context.pmGetContextHostName()

        self.pmconfig.finalize_options()

    def execute(self):
        """ Fetch and report """
        # Debug
        if self.context.pmDebug(PM_DEBUG_APPL1):
            sys.stdout.write("Known config file keywords: " + str(self.keys) + "\n")
            sys.stdout.write("Known metric spec keywords: " + str(self.pmconfig.metricspec) + "\n")

        # Set delay mode, interpolation
        if self.context.type != PM_CONTEXT_ARCHIVE:
            self.delay = 1
            self.interpol = 1

        # Common preparations
        self.context.prepare_execute(self.opts, False, self.interpol, self.interval)

        # Headers
        if self.header == 1:
            self.header = 0
            self.write_header()

        # Just checking
        if self.check == 1:
            return

        # Daemonize when requested
        if self.daemonize == 1:
            self.opts.daemonize()

        # Align poll interval to host clock
        if self.context.type != PM_CONTEXT_ARCHIVE and self.opts.pmGetOptionAlignment():
            align = float(self.opts.pmGetOptionAlignment()) - (time.time() % float(self.opts.pmGetOptionAlignment()))
            time.sleep(align)

        # Main loop
        while self.samples != 0:
            # Fetch values
            try:
                self.pmfg.fetch()
            except pmapi.pmErr as error:
                if error.args[0] == PM_ERR_EOL:
                    break
                raise error

            # Watch for endtime in uninterpolated mode
            if not self.interpol:
                if float(self.pmfg_ts().strftime('%s')) > float(self.opts.pmGetOptionFinish()):
                    break

            # Report and prepare for the next round
            self.report(self.pmfg_ts())
            if self.samples and self.samples > 0:
                self.samples -= 1
            if self.delay and self.interpol and self.samples != 0:
                self.pmconfig.pause()

        # Allow to flush buffered values / say goodbye
        self.report(None)

    def report(self, tstamp):
        """ Report the metric values """
        if tstamp != None:
            tstamp = tstamp.strftime(self.timefmt)

        self.write_es(tstamp)

    def write_header(self):
        """ Write info header """
        if self.context.type == PM_CONTEXT_ARCHIVE:
            sys.stdout.write("Sending %d archived metrics to Elasticsearch at %s...\n(Ctrl-C to stop)\n" % (len(self.metrics), self.es_server))
            return

        sys.stdout.write("Sending %d metrics to Elasticsearch at %s every %d sec" % (len(self.metrics), self.es_server, self.interval))
        if self.runtime != -1:
            sys.stdout.write(":\n%s samples(s) with %.1f sec interval ~ %d sec runtime.\n" % (self.samples, float(self.interval), self.runtime))
        elif self.samples:
            duration = (self.samples - 1) * float(self.interval)
            sys.stdout.write(":\n%s samples(s) with %.1f sec interval ~ %d sec runtime.\n" % (self.samples, float(self.interval), duration))
        else:
            sys.stdout.write("...\n(Ctrl-C to stop)\n")

    def write_es(self, timestamp):
        """ Write (send) metrics to an Elasticsearch host """
        if timestamp is None:
            # Silent goodbye
            return

        ts = self.context.datetime_to_secs(self.pmfg_ts(), PM_TIME_MSEC)

        try:
            es = Elasticsearch(hosts=[self.es_server])
            # pylint: disable=unexpected-keyword-arg
            es.indices.create(index=self.es_index,
                              ignore=[400],
                              body={'mappings':{'pcp-metric':
                                                {'properties':{'@timestamp':{'type':'date'},
                                                               'host-id':{'type':'string'}}}}})
        except ElasticsearchException as error:
            sys.stderr.write("Can't connect to Elasticsearch server %s: %s, continuing.\n" % (self.es_server, str(error)))
            return

        # Assemble all metrics into a single document
        # Use @-prefixed keys for metadata not coming in from PCP metrics
        es_doc = {'@host-id': self.es_hostid, '@timestamp': long(ts)}

        insts_key = "@instances"
        inst_key = "@id"

        for metric in self.metrics:
            try:
                # Install value into outgoing json/dict in key1{key2{key3=value}} style:
                # foo.bar.baz=value    =>  foo: { bar: { baz: value ...} }
                # foo.bar.noo[0]=value =>  foo: { bar: { @instances:[{@id: 0, noo: value ...} ... ]}

                pmns_parts = metric.split(".")

                # Find/create the parent dictionary into which to insert the final component
                for inst, name, val in self.metrics[metric][5](): # pylint: disable=unused-variable
                    try:
                        value = val()
                        value = round(value, self.precision) if isinstance(value, float) else value
                    except:
                        continue

                    pmns_leaf_dict = es_doc

                    for pmns_part in pmns_parts[:-1]:
                        if pmns_part not in pmns_leaf_dict:
                            pmns_leaf_dict[pmns_part] = {}
                        pmns_leaf_dict = pmns_leaf_dict[pmns_part]
                    last_part = pmns_parts[-1]

                    if inst == PM_IN_NULL:
                        pmns_leaf_dict[last_part] = value
                    else:
                        if insts_key not in pmns_leaf_dict:
                            pmns_leaf_dict[insts_key] = []
                        insts = pmns_leaf_dict[insts_key]
                        # Find a preexisting {@id: name} object in there, if any
                        found = False
                        for j in range(1, len(insts)):
                            if insts[j][inst_key] == name:
                                insts[j][last_part] = value
                                found = True
                        if not found:
                            insts.append({inst_key: name, last_part: value})
            except:
                pass

        try:
            # pylint: disable=unexpected-keyword-arg
            es.index(index=self.es_index,
                     doc_type='pcp-metric',
                     timestamp=long(ts),
                     body=es_doc)
        except ElasticsearchException as error:
            sys.stderr.write("Can't send to Elasticsearch server %s: %s, continuing.\n" % (self.es_server, str(error)))
            return

    def finalize(self):
        """ Finalize and clean up """
        return

if __name__ == '__main__':
    try:
        P = pcp2elasticsearch()
        P.connect()
        P.validate_config()
        P.execute()
        P.finalize()

    except pmapi.pmErr as error:
        sys.stderr.write('%s: %s\n' % (error.progname(), error.message()))
        sys.exit(1)
    except pmapi.pmUsageErr as usage:
        usage.message()
        sys.exit(1)
    except IOError as error:
        if error.errno != errno.EPIPE:
            sys.stderr.write("%s\n" % str(error))
            sys.exit(1)
    except KeyboardInterrupt:
        sys.stdout.write("\n")
        P.finalize()
