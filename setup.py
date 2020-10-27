from distutils.core import setup, Extension

prac_mod = Extension('prac',
                     sources=['prac.c'])

setup(name='prac',
      version='1.0',
      description='TODO',
      ext_modules=[prac_mod])
