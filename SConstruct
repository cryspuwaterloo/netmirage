def doBuild(name, target, cflags):
	env = Environment()
	env.Append(CFLAGS = cflags)
	Export('env')
	Export('target')
	SConscript("src/SConstruct", variant_dir="build/"+name, duplicate=0)

if int(ARGUMENTS.get('debug', 0)):
	doBuild('debug', 'sneac-debug', '-g3')
else:
	doBuild('release', 'sneac', '-O3')