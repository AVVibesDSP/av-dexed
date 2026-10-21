# Parches a JUCE

Arreglos oficiales de JUCE que todavía no están en la versión que usa AV Dexed (8.0.14, submódulo `libs/JUCE`).
`juce-8.0.14-backports.patch` junta, en un solo parche, cinco commits de la rama `develop` de JUCE tal cual (están
uno por uno en `upstream/`, solo como referencia: el 0004 depende del 0002 y el 0003). CMake lo aplica una sola vez a
los archivos del submódulo (que entonces aparece como modificado en `git status`); si no entra ni estaba aplicado, la
configuración se detiene. Para no aplicarlo: `-DAVDEXED_JUCE_PATCHES=OFF`. Para quitarlo del submódulo:
`git -C libs/JUCE checkout .`

Cuando AV Dexed suba a una versión de JUCE que ya los traiga, se borran de aquí.

| En `upstream/` | Commit de JUCE (develop, 13 oct 2026) | Qué arregla |
|---|---|---|
| 0001 | `e9656aae50` NSViewComponentPeer: Attempt to repaint as soon as possible after vblank | Mac: un `repaint()` llega a la pantalla en el siguiente cuadro, no uno después. |
| 0002 | `66cea28b11` Direct2DHwndContext: MakeHwndPimpl::present() private | Windows: reorganización que necesitan los siguientes. |
| 0003 | `26d459e860` Windows: Accumulate dirty rects manually instead of using InvalidateRect | Windows: las zonas por repintar se juntan en JUCE en vez de en el sistema. |
| 0004 | `4d92ab1c11` Direct2D: Avoid rendering frames before swapchain becomes ready | Windows: con una GPU que no alcanza (GPUs integradas, ventanas grandes animadas) o monitores de distinta frecuencia, `Present1` bloqueaba el hilo principal: clics y arrastres retrasados, la interfaz «se arrastra» detrás del cursor, parpadeo. Reportado en el foro de JUCE por Minimal Audio y otros con JUCE 8.0.13. |
| 0005 | `5aa5e635eb` Direct2D: Treat any success code as having presented, instead of just S_OK | Windows: el arreglo anterior dejaba de funcionar después de un protector de pantalla, bloqueo o hibernación. |

Fuentes: https://forum.juce.com/t/direct2d-laggy-mouse-interaction-under-heavy-paint-load-on-low-end-gpus-8-0-13-develop/69161 y
https://forum.juce.com/t/sluggish-and-glitchy-ui-on-windows-with-juce-8-0-13/69043

La parte de Windows no se pudo compilar ni probar en la Mac donde se preparó (entra limpia en 8.0.14 y todos los
símbolos que usa existen ahí); la de Mac se probó con las herramientas de interfaz.
