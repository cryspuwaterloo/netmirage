def doBuild(name, target, cflags=None, linkflags=None):
	env = Environment()
	if cflags is not None:
		env.Append(CFLAGS = cflags)
	if linkflags is not None:
		env.Append(LINKFLAGS = linkflags)
	Export('env')
	Export('target')
	SConscript("src/SConstruct", variant_dir="build/"+name, duplicate=0)

if int(ARGUMENTS.get('debug', 0)):
	doBuild('debug', 'netmirage-debug', '-g3 -DDEBUG')
else:
	doBuild('release', 'netmirage', '-O3 -flto -fno-fat-lto-objects', '-O3 -flto')
