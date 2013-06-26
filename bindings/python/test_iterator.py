#!/usr/bin/python
# -*- coding: utf-8 -*-

import sys

sys.path.insert(0, "bindings/python/")
import elliptics

def range(key_begin, key_end):
    ret = elliptics.IteratorRange()
    ret.key_begin = key_begin
    ret.key_end = key_end
    return ret

if __name__ == '__main__':
    log = elliptics.Logger("/dev/stderr", 1)
    cfg = elliptics.Config()
    cfg.cookie = "0123456789012345678901234567890123456789"
    cfg.config.wait_timeout = 60

    n = elliptics.Node(log, cfg)
    n.add_remote("localhost", 1025)

    s = elliptics.Session(n)
    s.add_groups([2])

    ranges = [range([0] * 64, [100] + [255] * 63), range([200] + [0] * 63, [220] + [255] * 63)]

    eid = elliptics.Id([0] * 64, 2)
    iterator = s.start_iterator(eid, ranges, \
                                elliptics.iterator_types.network, \
                                elliptics.iterator_flags.key_range \
                                    | elliptics.iterator_flags.ts_range \
                                    | elliptics.iterator_flags.data, \
                                elliptics.Time(0, 0), \
                                elliptics.Time(2**64-1, 2**64-1))

    for i, result in enumerate(iterator):
        if result.status != 0:
            raise AssertionError("Wrong status: {0}".format(result.status))

        print "key: {0}, flags: {1}, ts: {2}/{3}, data: {4}".format(
            result.response.key,
            result.response.user_flags,
            result.response.timestamp.tsec, result.response.timestamp.tnsec,
            result.response_data)

        # Test flow control
        if i % 10 == 0:
            print "Pause iterator"
            pause_it = s.pause_iterator(eid, result.id)
            print "Continue iterator"
            cont_it = s.continue_iterator(eid, result.id)