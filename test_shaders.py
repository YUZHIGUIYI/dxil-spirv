#!/usr/bin/env python3

#
# Copyright 2019 Hans-Kristian Arntzen for Valve Corporation
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#

import sys
import os
import os.path
import subprocess
import tempfile
import re
import itertools
import hashlib
import shutil
import argparse
import codecs
import json
import multiprocessing
import errno
from functools import partial

class Paths():
    def __init__(self, dxc, dxil_spirv):
        self.dxc = dxc 
        self.dxil_spirv = dxil_spirv 

def remove_file(path):
    os.remove(path)

def create_temporary(suff = ''):
    f, path = tempfile.mkstemp(suffix = suff)
    os.close(f)
    return path

def get_sm(shader):
    _, ext = os.path.splitext(shader)
    if ext == '.vert':
        return 'vs_6_0'
    elif ext == '.frag':
        return 'ps_6_0'
    elif ext == '.comp':
        return 'cs_6_0'
    elif ext == '.tesc':
        return 'hs_6_0'
    elif ext == '.tese':
        return 'ds_6_0'
    elif ext == '.geom':
        return 'gs_6_0'
    else:
        return ''

def cross_compile_dxil(shader, args, paths):
    dxil_path = create_temporary()
    glsl_path = create_temporary(os.path.basename(shader))
    dxil_cmd = [paths.dxc, '-Qstrip_reflect', '-Qstrip_debug', '-Vd', '-T' + get_sm(shader), '-Fo', dxil_path, shader]
    subprocess.check_call(dxil_cmd)

    glsl_cmd = [paths.dxil_spirv, '--output', glsl_path, '--glsl-embed-asm', '--glsl', '--validate', dxil_path]
    subprocess.check_call(glsl_cmd)
    return (dxil_path, glsl_path)

def make_unix_newline(buf):
    decoded = codecs.decode(buf, 'utf-8')
    decoded = decoded.replace('\r', '')
    return codecs.encode(decoded, 'utf-8')

def md5_for_file(path):
    md5 = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: make_unix_newline(f.read(8192)), b''):
            md5.update(chunk)
    return md5.digest()

def make_reference_dir(path):
    base = os.path.dirname(path)
    if not os.path.exists(base):
        os.makedirs(base)

def reference_path(directory, relpath, opt):
    split_paths = os.path.split(directory)
    reference_dir = os.path.join(split_paths[0], 'reference/' + ('opt/' if opt else ''))
    reference_dir = os.path.join(reference_dir, split_paths[1])
    return os.path.join(reference_dir, relpath)

def regression_check(shader, glsl, args):
    reference = reference_path(shader[0], shader[1], args.opt)
    joined_path = os.path.join(shader[0], shader[1])
    print('Reference shader path:', reference)

    if os.path.exists(reference):
        if md5_for_file(glsl) != md5_for_file(reference):
            if args.update:
                print('Generated source code has changed for {}!'.format(reference))
                # If we expect changes, update the reference file.
                if os.path.exists(reference):
                    remove_file(reference)
                make_reference_dir(reference)
                shutil.move(glsl, reference)
            else:
                print('Generated source code in {} does not match reference {}!'.format(glsl, reference))
                with open(glsl, 'r') as f:
                    print('')
                    print('Generated:')
                    print('======================')
                    print(f.read())
                    print('======================')
                    print('')

                # Otherwise, fail the test. Keep the shader file around so we can inspect.
                if not args.keep:
                    remove_file(glsl)
                raise RuntimeError('Does not match reference')
        else:
            remove_file(glsl)
    else:
        print('Found new shader {}. Placing generated source code in {}'.format(joined_path, reference))
        make_reference_dir(reference)
        shutil.move(glsl, reference)

def test_shader(shader, args, paths):
    joined_path = os.path.join(shader[0], shader[1])

    print('Testing shader:', joined_path)
    dxil, glsl = cross_compile_dxil(joined_path, args, paths)

    regression_check(shader, glsl, args)
    remove_file(dxil)

def test_shader_file(relpath, args):
    paths = Paths(args.dxc, args.dxil_spirv)
    try:
        test_shader((args.folder, relpath), args, paths)
        return None
    except Exception as e:
        return e

def test_shaders(args):
    all_files = []
    for root, dirs, files in os.walk(os.path.join(args.folder)):
        files = [ f for f in files if not f.startswith(".") ]   #ignore system files (esp OSX)
        for i in files:
            path = os.path.join(root, i)
            relpath = os.path.relpath(path, args.folder)
            all_files.append(relpath)

    # The child processes in parallel execution mode don't have the proper state for the global args variable, so
    # at this point we need to switch to explicit arguments
    if args.parallel:
        pool = multiprocessing.Pool(multiprocessing.cpu_count())

        results = []
        for f in all_files:
            results.append(pool.apply_async(test_shader_file,
                args = (f, args)))

        for res in results:
            error = res.get()
            if error is not None:
                pool.close()
                pool.join()
                print('Error:', error)
                sys.exit(1)
    else:
        for i in all_files:
            e = test_shader_file(i, args)
            if e is not None:
                print('Error:', e)
                sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description = 'Script for regression testing.')
    parser.add_argument('folder',
            help = 'Folder containing shader files to test.')
    parser.add_argument('--update',
            action = 'store_true',
            help = 'Updates reference files if there is a mismatch. Use when legitimate changes in output is found.')
    parser.add_argument('--keep',
            action = 'store_true',
            help = 'Leave failed GLSL shaders on disk if they fail regression. Useful for debugging.')
    parser.add_argument('--opt',
            action = 'store_true',
            help = 'Run DXC optimization passes as well.')
    parser.add_argument('--parallel',
            action = 'store_true',
            help = 'Execute tests in parallel.  Useful for doing regression quickly, but bad for debugging and stat output.')
    parser.add_argument('--dxc',
            default = './external/dxc-build/output/bin/dxc',
            help = 'Explicit path to DXC')
    parser.add_argument('--dxil-spirv',
            default = './dxil-spirv',
            help = 'Explicit path to dxil-spirv')

    args = parser.parse_args()
    if not args.folder:
        sys.stderr.write('Need shader folder.\n')
        sys.exit(1)

    if args.parallel and args.update:
        sys.stderr.write('Parallel execution is disabled when using the flags --update.\n')
        args.parallel = False

    test_shaders(args)
    print('Tests completed!')

if __name__ == '__main__':
    main()
