#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate a small output-configuration probe with every event handled."""
import pathlib
import sys
import xml.etree.ElementTree as ET

out = pathlib.Path(sys.argv[1])
interfaces = {}
for path in sys.argv[2:]:
    for interface in ET.parse(path).getroot().findall('interface'):
        interfaces[interface.attrib['name']] = interface

code = ['''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "device-client.h"
#include "management-client.h"
struct device { struct kde_output_device_v2 *object; char name[128]; } devices[8];
struct mode { struct kde_output_device_mode_v2 *object; int device, w, h, hz; unsigned flags; } modes[2048];
int nd, nm, result;
struct wl_display *display;
struct kde_output_management_v2 *manager;
static const struct kde_output_device_v2_listener device_listener;
static const struct kde_output_device_mode_v2_listener mode_listener;
static int device_index(void *object) {
    for (int i=0; i<nd; i++) if (devices[i].object == object) return i;
    return -1;
}
static struct mode *mode_info(void *object) {
    for (int i=0; i<nm; i++) if (modes[i].object == object) return &modes[i];
    abort();
}
''']
handlers = {
    ('kde_output_device_registry_v2', 'output'): '''
    if (nd >= 8) abort();
    devices[nd++].object = output;
    kde_output_device_v2_add_listener(output, &device_listener, NULL);''',
    ('kde_output_device_v2', 'name'): 'snprintf(devices[device_index(object)].name, 128, "%s", name);',
    ('kde_output_device_v2', 'mode'): '''
    if (nm >= 2048) abort();
    modes[nm++] = (struct mode){.object=mode, .device=device_index(object)};
    kde_output_device_mode_v2_add_listener(mode, &mode_listener, NULL);''',
    ('kde_output_device_mode_v2', 'size'): 'mode_info(object)->w=width; mode_info(object)->h=height;',
    ('kde_output_device_mode_v2', 'refresh'): 'mode_info(object)->hz=refresh;',
    ('kde_output_device_mode_v2', 'flags'): 'mode_info(object)->flags=flags;',
    ('kde_output_device_mode_v2', 'removed'): 'mode_info(object)->object=NULL;',
    ('kde_output_configuration_v2', 'applied'): 'result=1;',
    ('kde_output_configuration_v2', 'failed'): 'result=-1;',
    ('kde_output_configuration_v2', 'failure_reason'): 'fprintf(stderr, "failure: %s\\n", reason);',
}
listener_names = {
    'kde_output_device_registry_v2': 'registry_listener',
    'kde_output_device_v2': 'device_listener',
    'kde_output_device_mode_v2': 'mode_listener',
    'kde_output_configuration_v2': 'configuration_listener',
}
for name, listener in listener_names.items():
    events = interfaces[name].findall('event')
    for event in events:
        ename = event.attrib['name']
        args = ['void *data', f'struct {name} *object']
        for arg in event.findall('arg'):
            kind = arg.attrib['type']
            ctype = {'uint': 'uint32_t', 'int': 'int32_t', 'fixed': 'wl_fixed_t',
                     'string': 'const char *', 'array': 'struct wl_array *', 'fd': 'int'}.get(kind)
            if kind in ('new_id', 'object'):
                ctype = 'struct ' + arg.attrib['interface'] + ' *'
            args.append(ctype + ' ' + arg.attrib['name'])
        code.append(f'static void {name}_{ename}_event({", ".join(args)}) {{')
        code.append(handlers.get((name, ename), ''))
        code.append('}')
    code.append(f'static const struct {name}_listener {listener} = {{')
    for event in events:
        ename = event.attrib['name']
        code.append(f'.{ename} = {name}_{ename}_event,')
    code.append('};')
code.append('''
static void global(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t version) {
    if (!strcmp(name, "kde_output_management_v2")) {
        if (version < 24) abort();
        manager=wl_registry_bind(r, id, &kde_output_management_v2_interface, 24);
    } else if (!strcmp(name, "kde_output_device_registry_v2")) {
        struct kde_output_device_registry_v2 *devices=wl_registry_bind(r, id, &kde_output_device_registry_v2_interface, 24);
        kde_output_device_registry_v2_add_listener(devices, &registry_listener, NULL);
    }
}
static void global_remove(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener globals={global, global_remove};
static struct kde_output_device_mode_v2 *choose(int device, unsigned flag) {
    struct mode *best=NULL;
    for (int i=0; i<nm; i++) {
        struct mode *m=&modes[i];
        if (m->object && m->device==device && m->w==640 && m->h==480 && (m->flags & ~0x4000u)==flag)
            if (!best || ((flag & 0xc0000) ? m->hz > best->hz : abs(m->hz-60000)<abs(best->hz-60000))) best=m;
    }
    if (!best) { fprintf(stderr,"no mode device=%d flag=%x\\n", device, flag); exit(2); }
    printf("selected device=%d size=%dx%d hz=%d flags=%x\\n", device, best->w, best->h, best->hz, best->flags);
    return best->object;
}
int main(int argc, char **argv) {
    display=wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &globals, NULL);
    for (int i=0; i<4; i++) if (wl_display_roundtrip(display)<0) return 1;
    if (!manager || nd<1 || argc<2) return 1;
    if (nd==2 && strcmp(devices[0].name,devices[1].name)>0) {
        struct device tmp=devices[0]; devices[0]=devices[1]; devices[1]=tmp;
        for (int i=0;i<nm;i++) modes[i].device=1-modes[i].device;
    }
    printf("devices=%d modes=%d\\n", nd, nm);
    for (int i=0; i<nd; i++) printf("device %d %s\\n", i, devices[i].name);
    if (!strcmp(argv[1], "list")) {
        for (int i=0; i<nm; i++) printf("mode device=%d %dx%d hz=%d flags=%x\\n", modes[i].device,modes[i].w,modes[i].h,modes[i].hz,modes[i].flags);
        return 0;
    }
    struct kde_output_configuration_v2 *c=kde_output_management_v2_create_configuration(manager);
    kde_output_configuration_v2_add_listener(c,&configuration_listener,NULL);
    if (!strcmp(argv[1],"enable")) {
        for (int i=0;i<nd;i++) kde_output_configuration_v2_set_stereo_formats(c,devices[i].object,1,1);
    } else if (!strcmp(argv[1],"mode")) {
        unsigned flag=strtoul(argv[2],NULL,0);
        kde_output_configuration_v2_enable(c,devices[0].object,1);
        kde_output_configuration_v2_set_stereo_pair(c,devices[0].object,NULL,0,0,0);
        kde_output_configuration_v2_mode(c,devices[0].object,choose(0,flag));
        for (int i=1;i<nd;i++) {
            kde_output_configuration_v2_set_stereo_pair(c,devices[i].object,NULL,0,0,0);
            kde_output_configuration_v2_enable(c,devices[i].object,0);
        }
    } else if (!strcmp(argv[1],"pair") && argc==5 && nd==2) {
        int kind=atoi(argv[2]);
        for (int i=0;i<2;i++) {
            kde_output_configuration_v2_enable(c,devices[i].object,1);
            kde_output_configuration_v2_mode(c,devices[i].object,choose(i,0));
            kde_output_configuration_v2_position(c,devices[i].object,0,0);
            kde_output_configuration_v2_set_stereo_pair(c,devices[i].object,devices[1-i].object,kind,(kind==3?2:0)+i,atoi(argv[3+i]));
        }
    } else return 2;
    kde_output_configuration_v2_apply(c);
    while (!result && wl_display_dispatch(display)>=0) {}
    printf("configuration=%s\\n",result==1?"applied":"failed");
    return result==1?0:1;
}
''')
out.write_text('\n'.join(code))
