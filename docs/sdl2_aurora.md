# SDL2 под ОС Аврора (форк с фиксами)

Этот документ читай, только если портируемый проект использует SDL2
(окно, ввод, звук через SDL). Здесь собрано: какую версию SDL2 брать,
как собрать её в пакете, и SDL2-специфичные API, нужные на этапах
поворота контента (ориентация дисплея, `set_buffer_transform`).

## Какую версию брать

При сборке пакета использовать ТОЛЬКО эту версию SDL2 — в ней фиксы
под ОС Аврора:

- https://github.com/savegame/SDL

Репозиторий выкачивается как подмодуль в папку `libsdl`.

## Правило: libsdl не трогать

ЗАПРЕЩЕНО трогать libsdl (встроенный SDL2): не откатывать и не
«чинить» тамошние патчи (в т.ч. `c27776d4e` «Do not swap width and
height, the lipstick can't stand it»). Патч, запрещающий пересчёт
размеров по output transform, — верный и осознанный: SDL должен
отдавать реальные размеры панели. Если поведение SDL кажется
неправильным — эскалация пользователю, а не правка SDL.

## Сборка в пакете

1. В spec-файле добавить следующие `BuildRequires`:

```
BuildRequires: pkgconfig(wayland-client)
BuildRequires: pkgconfig(wayland-cursor)
BuildRequires: pkgconfig(wayland-egl)
BuildRequires: pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(wayland-scanner)
BuildRequires: pkgconfig(glesv2)
BuildRequires: pkgconfig(xkbcommon)
BuildRequires: pkgconfig(vulkan)
BuildRequires: pkgconfig(egl)
BuildRequires: pkgconfig(sdl2)
BuildRequires: pkgconfig(libpulse)
```

2. В секции `%build`, перед сборкой основного проекта, добавить скрипт:

```
cmake \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -Bbuild/%{_arch}/libsdl \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_PULSEAUDIO=ON \
    -DSDL_RPATH=OFF \
    -DSDL_AUDIO=ON \
    -DSDL_STATIC=OFF \
    -DSDL_WAYLAND=ON \
    -DSDL_X11=OFF \
    -DSDL_WAYLAND_LIBDECOR=OFF \
    libsdl

cmake --build build/%{_arch}/libsdl -j`nproc`
pushd build/%{_arch}/libsdl
rsync -avP include-config-release/SDL2/* include/SDL2/
rsync -avP *.so* lib/
popd
```

3. В `%install` добавить установку:

```
install -D -s build/%{_arch}/libsdl/libSDL2-2.0.so* -t %{buildroot}%{_datadir}/%{name}/lib
```

## События ориентации дисплея

SDL2 интерпретирует поворот сама: она уже разобрала
`wl_output::transform` с учётом типа экрана и присылает готовую
ориентацию. НЕ интерпретируй `wl_output::transform` повторно, если
движок использует события SDL2. Сначала проверь в коде движка,
обрабатывает ли он `SDL_DISPLAYEVENT_ORIENTATION` и как.

- Событие: `SDL_DISPLAYEVENT` с подтипом
  `SDL_DISPLAYEVENT_ORIENTATION`; `event.display.data1` содержит
  `SDL_DisplayOrientation`:
  `SDL_ORIENTATION_UNKNOWN`, `SDL_ORIENTATION_LANDSCAPE`,
  `SDL_ORIENTATION_LANDSCAPE_FLIPPED`, `SDL_ORIENTATION_PORTRAIT`,
  `SDL_ORIENTATION_PORTRAIT_FLIPPED`.
- Текущее значение также доступно через
  `SDL_GetDisplayOrientation(displayIndex)`.

Тип панели (портретная/ландшафтная) узнаётся по нативному разрешению
текущего дисплея в пикселях: `SDL_GetCurrentDisplayMode()` по индексу
дисплея окна (`SDL_GetWindowDisplayIndex()`): H > W — портретная
панель (телефон), W > H — ландшафтная (планшет, монитор, ТВ).
Определяется один раз при инициализации и пересчитывается ТОЛЬКО при
смене дисплея — см. «Маппинг ориентаций под ориентацию игры» в
`docs/fbo_module.md`.

Учти: при повороте события изменения размера
(`SDL_WINDOWEVENT_SIZE_CHANGED`) НЕ приходят — см. подраздел
«Размер окна и буфера при повороте» в `docs/fbo_module.md`.
Обработчики resize движка нужно вызывать вручную при реальной
смене размеров буфера.

## set_buffer_transform из SDL2-приложения

Чтобы выставить трансформацию буфера из приложения на SDL2:

1. Бинарь должен линковаться с wayland-client — в spec-файле нужен
   `BuildRequires: pkgconfig(wayland-client)` (он уже входит в список
   зависимостей SDL2 выше).
2. В файле, где дёргается API set_buffer_transform, добавить:
   `#include <wayland-client.h>`
3. Передавать трансформацию буфера (псевдокод как пример; в конкретном
   случае `orientation` может быть enum движка или реальным углом
   поворота в градусах):

```c
struct wl_surface *surface = NULL;
SDL_SysWMinfo wmInfo;

if( !sdlWindow ) // SDL_Window pointer
	return;

SDL_VERSION( &wmInfo.version );
if( !SDL_GetWindowWMInfo( sdlWindow, &wmInfo ))
	return;
if( wmInfo.subsystem != SDL_SYSWM_WAYLAND )
	return;

surface = wmInfo.info.wl.surface;
if( !surface )
	return;

switch( orientation )
{
case landscape:
	wl_surface_set_buffer_transform( surface, WL_OUTPUT_TRANSFORM_90 );
	break;
case inverted_portrait:
	wl_surface_set_buffer_transform( surface, WL_OUTPUT_TRANSFORM_180 );
	break;
case inverted_landscape:
	wl_surface_set_buffer_transform( surface, WL_OUTPUT_TRANSFORM_270 );
	break;
default: // portrait
	wl_surface_set_buffer_transform( surface, WL_OUTPUT_TRANSFORM_NORMAL );
	break;
}
```

Одно и то же значение `wl_output_transform` идёт и в поворот квада
(`set_rotation` FBO-модуля), и в `wl_surface_set_buffer_transform` —
см. «Требования к FBO-модулю» в `docs/fbo_module.md`.

## Размер окна для лаунчера/окна приложения

Для полноэкранного окна размер берите СНАЧАЛА из
`SDL_GetDisplayUsableBounds` (на портретной панели это уже с учётом
transform композитора), и только как fallback —
`SDL_GetDesktopDisplayMode`: голый desktop mode может вернуть
нативный ландшафт панели без поворота, и окно получится с неверной
ориентацией (подробности — в `docs/imgui_launcher.md`).
