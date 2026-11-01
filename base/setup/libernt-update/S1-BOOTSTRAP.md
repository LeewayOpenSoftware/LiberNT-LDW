# S1-BOOTSTRAP: bootstrap online/offline de LiberNT Update (etapa 1)

Autoría: Kevin (card `libernt-update-s1-bootstrap`, 2026-10-30). Implementa
DESIGN-v0 §9 bajo OWNER-STAGE1-CONDITIONS.md. Nombres provisionales
(condición 1). Nada de esto entra a la ISO.

## Qué es

`libernt-update-bootstrap.exe` instala o desinstala la infraestructura de
LiberNT Update sobre un LiberNT existente, en marcha (online) o sobre una
raíz montada/apagada (offline, `--image <root>`), sin reinstalar el sistema
(condición: "instalable y desinstalable online u offline").

```
libernt-update-bootstrap.exe install   --bundle <dir> [--image <root>] [--dry-run]
libernt-update-bootstrap.exe uninstall [--image <root>] [--export <dir>] [--dry-run]
```

## El bundle

Carpeta con el manifiesto firmado y los binarios. El manifiesto se verifica
con la clave pública de DESARROLLO embebida (`s1core`, RSA-2048 PKCS#1 v1.5
sobre SHA-256 — la misma raíz de confianza que S1-CONTRACTS §1b, condición 2
enforce sin modo audit). Un bundle cuya firma no verifica se rechaza antes
de tocar el target.

```
bundle/
  bootstrap-manifest.json
  bootstrap-manifest.sig     RSA-2048/PKCS1v15/SHA-256 sobre los bytes exactos del .json
  files/<...>
```

`bootstrap-manifest.json`:

```json
{
  "schema": 1,
  "binaries": [
    {"name": "...", "source": "files/pending.exe",
     "dest": "Windows\\System32\\LiberNTUpdatePending.exe",
     "sha256": "<64 hex>", "required": true}
  ]
}
```

- `dest` es relativo a la raíz del target. Solo se aceptan destinos dentro
  de `Windows\...` o `LiberNT\...` (allowlist deliberada; sin rutas
  arbitrarias, DESIGN §4).
- `required: true` ⇒ el archivo debe existir en el bundle; si falta, no se
  instala nada.
- Entrada con `"service": true` ⇒ su `dest` es el binario del servicio
  (registrado online; offline queda como stub declarado).

## Qué instala

1. `TARGET\LiberNT\Servicing\` con el árbol `store`, `journal`, `pending`,
   `backup`, `bin`, `logs`, `bootstrap`. Cada directorio se crea con el DACL
   protegido de la revisión B1 de Pam: owner SYSTEM, DACL heredable
   protegida con únicamente ACEs de allow para SYSTEM y Administrators
   (SDDL `O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`). Es la **misma regla**
   que `CheckSecure`/`CreateSecureDir` de libservicing (patch 0005 de
   Meredith); el bootstrap la aplica de forma idéntica y, si el árbol ya
   existe, exige que la cumpla antes de escribir (fail closed: sin ACLs,
   como en FAT, se rechaza).
2. Los binarios del bundle en sus `dest`, copiados y verificados contra el
   `sha256` del manifiesto (el hash se recalcula en la copia y en el
   self-test; un byte distinto aborta con `ERROR_CRC`).
3. `store\dev-public-key.bin`: clave pública DEV embebida fijada en el store
   protegido (pinning, S1-CONTRACTS §1b).
4. `HKLM\SYSTEM\<CurrentControlSet|ControlSet00N>\Control\Session Manager\
   BootExecute` (REG_MULTI_SZ): añade `LiberNTUpdatePending` **después** de
   las entradas `autocheck`/`autochk`, preservando el orden existente y sin
   duplicar (idempotente). Offline: el hive `SYSTEM` del target se carga con
   `RegLoadKeyW` (advapi32 reg.c:3134), se resuelve el control set por
   `Select\Current` (las cargas offline no tienen el enlace simbólico
   `CurrentControlSet`), se edita y se descarga con `RegUnLoadKeyW`.
5. Servicio `LiberNTUpdateSvc` (solo online, y solo si el bundle trae el
   binario con `"service": true`): se crea **deshabilitado**, y solo se
   habilita (demand start) si el self-test pasa. Si el bundle no trae el
   servicio, queda un **stub explícito**: `installed.json` lo declara como
   `"service":"stub"` y no se registra nada.
6. `bootstrap\installed.json`: inventario de todo lo que el bootstrap
   instaló (archivos + hashes, servicio, BootExecute). Es la única fuente
   que la desinstalación borra: nunca se toca un archivo ajeno.

El bootstrap **no toca el kernel ni SMSS** (condición 5/DESIGN §9) y no
ejecuta nada del target en modo `--image`.

## Self-test (post-instalación, antes de habilitar el servicio)

1. Re-lee cada archivo instalado y recalcula SHA-256 contra el inventario.
2. `CheckSecure` sobre el árbol de servicing.
3. La clave fijada en el store es idéntica a la embebida.
4. Transacción simulada sobre un archivo temporal del propio bootstrap
   (crear/mover/restaurar/borrar) ejercitando las primitivas de backup.
5. Online con servicio: `OpenService`+query.

Si falla, el servicio queda deshabilitado y la salida lo dice: no es seguro
reiniciar en modo update.

## Desinstalación

1. Rechaza si existe `Servicing\pending\plan.json` (transacción activa:
   hay que dejarla aplicar o revertir primero; diagnóstico, no borrado de
   datos compartidos, DESIGN §9).
2. Quita la entrada `BootExecute` (offline: misma carga de hive).
3. Detiene y borra el servicio (online; solo si `installed.json` dice
   `installed`).
4. Borra únicamente los archivos del inventario + `dev-public-key.bin` +
   `installed.json`. Los logs y el journal **se conservan**; con `--export
   <dir>` se copian al medio externo antes del borrado.

## Idempotencia e interrupciones

Reejecutar `install` sobre un target ya instalado: los archivos se
re-verifican contra el bundle (mismo hash ⇒ se reescriben, es barato y
garantiza el estado final), BootExecute no se duplica, el servicio ya
registrado se mantiene. Una interrupción a mitad deja como mucho archivos
copiados + hive sin editar o editado: reejecutar converge al mismo estado
(las operaciones son ordenadas: árbol → archivos → clave → BootExecute →
servicio → self-test).

## Validaciones de entrada

- Bundle: manifiesto parseable, firma válida, archivos `required` presentes.
- Online: el target es NTFS (los DACL protegidos no existen en FAT);
  privilegio `SeRestorePrivilege` (para fijar owner SYSTEM).
- `--image`: la raíz contiene `Windows\System32\config\SYSTEM`; espacio
  libre ≥ 4× el bundle (mínimo 32 MiB).
- TODO(condición: volumen exclusivo): si `--image` apunta a la raíz de un
  volumen (`X:`), bloquear el volumen con `FSCTL_LOCK_VOLUME` durante la
  operación. Hoy el lock de volumen está preparado en la estructura del
  target pero no se activa; documentado como límite de la etapa 1.

## Dependencias verificadas en el árbol

- `RegLoadKeyW`/`RegSaveKeyW`/`RegUnLoadKeyW`/`RegSetValueExW`/
  `RegOpenKeyExW`: `dll/win32/advapi32/reg/reg.c` (3134, 4935, ~5397, 5176,
  1785).
- `GetNamedSecurityInfoW`/`SetNamedSecurityInfoW`:
  `dll/win32/advapi32/sec/misc.c` (1173, 1223).
- `ConvertStringSecurityDescriptorToSecurityDescriptorW`:
  `dll/win32/advapi32/wine/security.c` (2671).
- `OpenSCManagerW`/`CreateServiceW`: `dll/win32/advapi32/service/scm.c`
  (2105, 812).
- `s1core` (SHA-256 + verificación RSA): `base/system/servicing/pending/`
  (importado en esta rama desde `feature/libernt-update-s1-pending`,
  `bfa62cef6d`, revisión cripto de Pam sin bloqueantes).

## Pruebas

- En el árbol: target `libernt-update-bootstrap` (amd64); no se agrega al
  ISO ni a `bootcd` (condición del dueño).
- En el host: `tests/hosttest.py` (python 3, requiere administrador)
  construye un bundle firmado con la clave DEV (`%USERPROFILE%\.leeway-dev-signing\`,
  fuera del repo), una raíz falsa y un hive SYSTEM generado con `RegSaveKey`, y
  verifica: dry-run sin escrituras, rechazo de bundle sin firma, instalación
  byte-exacta, orden e idempotencia de BootExecute en el hive, rechazo de
  desinstalación con plan pendiente, desinstalación limpia con exportación
  de logs.

## Límites conocidos (etapa 1)

- El lock de volumen exclusivo en `--image` no está activado (TODO arriba).
- Un paquete mixto (items fuera de los directorios de sistema aplicados en
  línea + items de sistema diferidos al arranque) queda **parcialmente
  aplicado** hasta el reinicio, igual que la rama "destino en uso". La regla
  de ruta del engine diferente a boot todo reemplazo de un archivo existente
  bajo `Windows`, `Program Files` o `Program Files (x86)`.
- El registro del servicio solo existe online; offline queda el stub
  declarado (DESIGN §9: el modo offline no necesita el servicio).
- El bootstrap escribe el log en `Servicing\logs\bootstrap.log` desde que el
  árbol existe; pasos anteriores van solo a consola.
- La rotación de la clave DEV (sustituir la embebida por otra) no está
  implementada: condición 6 prohíbe generar la clave de distribución sin
  autorización.

## Journal persistentemente inescribible (decisión A, corrida final S1)

Si el journal no se puede escribir en absoluto (por ejemplo el atributo
de solo lectura), cada arranque se congela **antes de tocar ningún
fichero**: la primera fase que se journaliza (`verified`) falla,
`S1JournalOrHalt` sale con exit 1 y smss continua (sminit.c:2585
descarta el estado del hijo). Propiedades garantizadas:

- **Nunca hay un target medio-aplicado**: el halt precede a toda
  operación de fichero; el plan queda intacto en `pending\plan.json`.
- **No es silencioso**: cada arranque imprime la línea `S1: journal
  append ... failed 0x..., halting` por el depurador de kernel, y el
  contador de intentos **no** avanza (un arranque que no pudo
  journalizar su verificación nunca intentó la transacción).
- **La recuperación es por el journal**: al devolver la escritura al
  journal (quitar el atributo, corregir la ACL), el siguiente arranque
  re-verifica y aplica el plan de nuevo (flujo (a)); el rollback por CLI
  y el desinstalador también necesitan el journal escribible, así que la
  única vía de salida ante un journal roto persistente es repararlo o
  retirar el plan en frío. Contar el halt como intento no ayudaría: el
  rollback también journaliza y se detendría igual.
