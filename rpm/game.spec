%define _app_orgname org.example
%define _app_appname gameport
%define _app_launcher_name Game Port

Name:       %{_app_orgname}.%{_app_appname}
Summary:    Game Port by AGENTS.md
Release:    1
Version:    1.0.0
Group:      Amusements/Games
License:    BSD3
Source0:    %{name}.tar.gz

%define __requires_exclude ^libexample.*\\.so.*|libexample\\.so.*$
%define __provides_exclude_from ^%{_datadir}/%{name}/lib/.*\\.so.*$

BuildRequires: rsync
BuildRequires: patchelf
BuildRequires: ninja

%description
"Game ported by AGENTS.md"

%prep
%setup -q -n %{name}-%{version}

%build
# here all build instructions


%install
# here all installs

# then patch elf path for game binary
patchelf --force-rpath --set-rpath %{_datadir}/%{name}/lib %{buildroot}%{_bindir}/%{name}
# install icons files
install -m 655 -D icons/86.png  %{buildroot}%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
install -m 655 -D icons/108.png %{buildroot}%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
install -m 655 -D icons/128.png %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
install -m 655 -D icons/172.png %{buildroot}%{_datadir}/icons/hicolor/172x172/apps/%{name}.png

sed "s/__ORGNAME__/%{_app_orgname}/g" game.desktop.in>%{name}.desktop
sed -i "s/__APPNAME__/%{_app_appname}/g" %{name}.desktop
sed -i "s/__X_APPLICATION__/%{firejail_section}/g" %{name}.desktop
sed -i "s/__LAUNCHER_NAME__/%{_app_launcher_name}/g" %{name}.desktop

install -m 655 -D %{name}.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop

# install game data files
# install -d %{buildroot}%{_datadir}/%{name}
# cp -r game/data %{buildroot}%{_datadir}/%{name}/

%files
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/%{name}
%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
%{_datadir}/icons/hicolor/172x172/apps/%{name}.png
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
