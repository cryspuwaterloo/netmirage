# Public version information
appVersion = {
	'name': 'NetMirage',
	'major': 0,
	'minor': 9,
	'revision': 0
}

env = Environment()

# Common build flags
debugMode = int(ARGUMENTS.get('debug', 0))
env.Append(CFLAGS = '-Wall -Wextra -Wundef -Wendif-labels -Wshadow -Wpointer-arith -Wbad-function-cast -Wcast-qual -Wcast-align -Wwrite-strings -Wconversion -Waggregate-return -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations -Wpacked -Wredundant-decls -Wnested-externs -Winline -Winvalid-pch -Wdisabled-optimization')
env.Append(CFLAGS = '-Wno-unused-parameter -Wno-missing-field-initializers')
env.Append(CFLAGS = '-Werror -fmax-errors=10 -std=c99')
buildDir = 'build'
if debugMode:
	env.Append(CFLAGS = '-g3 -DDEBUG')
	buildDir += '/debug'
	targetSuffix = '-debug'
else:
	env.Append(CFLAGS = '-O3 -flto -fno-fat-lto-objects')
	env.Append(LINKFLAGS = '-O3 -flto')
	buildDir += '/release'
	targetSuffix = ''

# Resolve glib dependency
env.ParseConfig('pkg-config --cflags --libs glib-2.0')

# Needed for link-time optimization
env.Replace(AR = 'gcc-ar')
env.Replace(RANLIB = 'gcc-ranlib')

# Configure build targets
Export('env')
Export('appVersion')
Export('targetSuffix')
SConscript('src/auto/SConstruct', variant_dir=buildDir+'/auto', duplicate=0)
SConscript('src/common/SConstruct', variant_dir=buildDir+'/common', duplicate=0)
SConscript('src/netmirage/SConstruct', variant_dir=buildDir+'/netmirage', duplicate=0)
