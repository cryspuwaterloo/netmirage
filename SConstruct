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
tarFiles = ['SConstruct', 'README', 'src']
tar = tarEnv.Tar(tarName, tarFiles)
tarEnv.Alias('tar', tar)
