project('egl-image-widget', 'c')

drm = dependency('libdrm')
epoxy = dependency('epoxy')
glu = dependency('glu')
gtk = dependency('gtk4')
x11_xcb = dependency('x11-xcb')
xcb_dri3 = dependency('xcb-dri3')

executable('example-gl2', 'example-gl2.c', 'gtkeglimagewidget.c',
           dependencies: [drm, epoxy, glu, gtk, x11_xcb, xcb_dri3])
