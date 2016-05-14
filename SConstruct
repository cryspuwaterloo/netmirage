################################################################################
 # Copyright (C) 2016 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
 # Foundation for Education, Science and Community Development.
 #
 # This file is part of NetMirage.
 #
 # NetMirage is free software: you can redistribute it and/or modify it under
 # the terms of the GNU Affero General Public License as published by the Free
 # Software Foundation, either version 3 of the License, or (at your option) any
 # later version.
 #
 # NetMirage is distributed in the hope that it will be useful, but WITHOUT ANY
 # WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 # A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 # details.
 #
 # You should have received a copy of the GNU Affero General Public License
 # along with NetMirage. If not, see <http://www.gnu.org/licenses/>.
 ###############################################################################

# Public version information
appVersion = {
	'major': 0,
	'minor': 9,
	'revision': 0
}

bareEnv = Environment()

# Common build flags
debugMode = int(ARGUMENTS.get('debug', 0))
bareEnv.Append(CFLAGS = '-Wall -Wextra -Wundef -Wendif-labels -Wshadow -Wpointer-arith -Wbad-function-cast -Wcast-qual -Wcast-align -Wwrite-strings -Wconversion -Waggregate-return -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations -Wpacked -Wredundant-decls -Wnested-externs -Winline -Winvalid-pch -Wdisabled-optimization')
bareEnv.Append(CFLAGS = '-Wno-unused-parameter -Wno-missing-field-initializers')
bareEnv.Append(CFLAGS = '-Werror -fmax-errors=10 -std=c99')
buildDir = 'build'
if debugMode:
	bareEnv.Append(CFLAGS = '-g3 -DDEBUG')
	buildDir += '/debug'
	targetSuffix = '-debug'
else:
	bareEnv.Append(CFLAGS = '-O3 -flto -fno-fat-lto-objects')
	bareEnv.Append(LINKFLAGS = '-O3 -flto')
	buildDir += '/release'
	targetSuffix = ''

# Needed for link-time optimization
bareEnv.Replace(AR = 'gcc-ar')
bareEnv.Replace(RANLIB = 'gcc-ranlib')

env = bareEnv.Clone()

# Common dependencies
env.ParseConfig('pkg-config --cflags --libs glib-2.0')

# Configure build targets
Export('bareEnv')
Export('env')
Export('appVersion')
Export('targetSuffix')
SConscript('src/auto/SConstruct', variant_dir=buildDir+'/auto', duplicate=0)
SConscript('src/common/SConstruct', variant_dir=buildDir+'/common', duplicate=0)
SConscript('src/netmirage-core/SConstruct', variant_dir=buildDir+'/netmirage-core', duplicate=0)
SConscript('src/netmirage-edge/SConstruct', variant_dir=buildDir+'/netmirage-edge', duplicate=0)

# Configure the tarball build target
tarName = 'netmirage-%d.%d.%d'%(appVersion['major'],appVersion['minor'],appVersion['revision'])
tarEnv = Environment(TARFLAGS = ('-c -z --transform \'s,^,%s/,\''%tarName), TARSUFFIX = '.tar.gz')
tarFiles = ['SConstruct', 'COPYING', 'LIBRARIES', 'README', 'src']
tar = tarEnv.Tar(tarName, tarFiles)
tarEnv.Alias('tar', tar)
