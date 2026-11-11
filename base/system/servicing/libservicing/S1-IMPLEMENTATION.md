# LiberNT Update etapa 1: implementacion del nucleo (libservicing + libernt-update.exe)

Autora: Meredith, 2026-10-30. Rama `feature/libernt-update-s1-core-meredith` sobre integration-1031 (483daf4db0). Interno de Leeway.
**Contrato unico: `shared/leeway/libernt-update/S1-CONTRACTS.md`** (formatos de plan, journal, attempt, transactionId y estados; incluye la seccion "Reconciliacion con S1-CORE.md"). `S1-CORE.md` (Oscar) queda superado. Condiciones del dueno: `OWNER-STAGE1-CONDITIONS.md`. Revision de seguridad: Pam (B1-B4, N1-N7).

**Estado: compilado y probado en host (Windows 11, wintrust real). No probado en la VM de LiberNT ni en la dx2400.** Los informes de compilacion y de pruebas van separados (secciones 8 y 9).

## 1. Piezas

| pieza | ubicacion | funcion |
|---|---|---|
| libservicing | `base/system/servicing/libservicing/` | manifest, store, journal, transacciones, verificacion ENFORCE, plan/attempt, targets Online/Image, arbol protegido |
| libernt-update.exe | `base/applications/cmdutils/libernt-update/` | CLI: init, query, verify, add-package, apply, confirm, rollback, remove-package, pending, export-log |
| scripts de host | `tools/libernt-update/` | `New-DevSigningCert.ps1`, `New-FixturePackage.ps1`, `Build-And-Test.ps1`, certificado publico en `certs/` |
| tests | `libservicing/tests/tests.c` | 412 comprobaciones (con ganchos `LN_TESTING`, solo en la copia de pruebas de la libreria) |

CMake propio y desconectado: ningun `CMakeLists.txt` padre incluye estas carpetas (ni hivesys.inf, ni la ISO, ni DISM).
El CLI y la libreria necesitan privilegios de administrador (o SYSTEM) para crear/abrir el arbol protegido.

## 2. Modelo de seguridad

El arbol `TARGET\LiberNT\Servicing` (store, ancla de confianza, journal, plan, backups) decide que instala el motor como administrador:
- se crea con DACL **protegido** SYSTEM + Administrators; al abrir un target se exige duenio SYSTEM/Administrators/TrustedInstaller, DACL protegido y solo ACE de permiso para esos principales (tambien en `TARGET\LiberNT`); si no, `LN_E_INSECURE_TARGET` antes de leer nada. Sin ACL (FAT) falla cerrado.
- `add-package` copia el paquete a un staging privado dentro del arbol protegido, verifica SOLO la copia e importa de ella.
- `apply` y `confirm` vuelven a verificar el paquete guardado (secciones 3 y 4).
- `pending` valida el plan contra el paquete firmado (`LnServicingValidatePlan`): digest, transactionId, y cada operacion contra el manifest.
- journal: linea no parseable solo si es la ultima y sin salto de linea; `journal\anchor.json` con `(seq, hash)` de la ultima entrada.

## 3. Verificacion ENFORCE (sin audit); la raiz de confianza es `manifest.sig`
Decision de arquitectura (god, 2026-10-30): UN solo esquema de confianza. `manifest.sig` = RSA-2048 PKCS#1 v1.5 sobre SHA-256 de `manifest.json`, hecha en el host al construir el paquete con la clave DEV. Se verifica con `S1Rsa2048Sha256Verify` del `s1core` portable de Kevin (`base/system/servicing/pending/s1core.{c,h}`, importado tal cual desde su rama; sin dependencias), el MISMO codigo que usa el aplicador de arranque. libservicing solo extrae modulo y exponente del certificado DER fijado (lector DER propio). `plan.json` no se firma. El `.cat` es opcional y adicional (solo drivers).

En este orden; el primer fallo corta y no escribe nada en el target:
1. `manifest.json`: esquema v1, ids `[A-Za-z0-9._-]` sin nombres de dispositivo ni `~`, hashes SHA-256, sin destinos duplicados, <= 4096 items -> `LN_E_MANIFEST_INVALID`.
2. Lista blanca de destinos -> `LN_E_FORBIDDEN_TARGET`: cualquier archivo (hash en el manifest firmado) bajo `Windows\`, `Program Files`, `Program Files (x86)` (orden del owner 2026-11-11, S1-PACKAGE-EXE-DESIGN.md seccion 1), salvo hives y sus logs (`System32\config`, `ServiceProfiles`, `ntuser.dat*`, `usrclass.dat*`, `*.regtrans-ms`, `*.blf`), `System32\boot`, `LiberNT\`, rosload, LiberNTUpdatePending.exe, y excepto smss, csrss, wininit, winlogon, services, lsass, ntdll, ci.dll, kernel, HAL, win32k*, bootvid, kdcom, autochk y la ruta de arranque; `.sys` solo bajo `System32\drivers` con `driverStart` 2..4 declarado en el manifest firmado. Sin `..`, raiz, unidad, `:`, componentes vacios o con punto/espacio final, nombres 8.3 (`~` + digito), nombres de dispositivo.
3. Cada payload existe (carpeta del paquete, o objeto del store) y su SHA-256 coincide con el manifest -> `LN_E_HASH_MISMATCH`.
4. El hash del certificado fijado en `trust\` == `certificateSha256` del manifest -> `LN_E_CERT_MISMATCH` / `LN_E_NO_TRUST_ANCHOR`.
5. `manifest.sig` verifica sobre `manifest.json` con la clave publica de ese certificado -> `LN_E_UNSIGNED` (ausente) / `LN_E_BAD_SIGNATURE`.
6. `catalog.cat` (opcional): si esta, debe verificar completo (WinVerifyTrust, miembros que cubren manifest y payloads, firmante == certificado fijado) -> `LN_E_UNSIGNED` / `LN_E_CATALOG_INVALID` / `LN_E_CERT_MISMATCH`; un paquete con payload de driver debe traerlo (`LN_E_UNSIGNED` si falta).

Del catalogo, el certificado DEV es autofirmado: `WinVerifyTrust` devuelve `CERT_E_UNTRUSTEDROOT` tras comprobar la firma y solo ese resultado se acepta, porque el firmante esta fijado. No se toca el almacen de raices del sistema. Certificado DEV: RSA-2048 (el `s1core` no verifica otros tamanos).

## 4. Transacciones (una transaccion `install` por paquete; contrato S1-CONTRACTS.md)
- **add-package:** stage -> verifica la copia -> `verified` -> importa objetos (`store\objects\<sha256>`), manifest, catalogo y firma -> comprueba que lo guardado es lo verificado -> `staged`. Un rechazo queda como `failed` con su codigo hex.
- **apply:** re-verifica el paquete del store, valida cada destino (sin reparse points, camino real == escrito), toma y verifica el backup de cada destino ANTES del primer cambio (`backup\<tx>\NNNN`, `undo.json`), `prepared` -> `applying`; por item: copia a temporal, hash, reemplazo atomico, hash final, evento `applying` con `op`. Todo bien -> `applied`. Un item falla -> `failed` + `rollback-prepared` (`verifiable-failure`) -> restaurar -> `rolled-back`. Destino en uso (Online): el item pasa al `plan.json` y la fase es `pending-first-boot` (un borrado de archivo en uso no se difiere: falla y revierte). `ACCESS_DENIED` cuenta como "en uso" solo para un archivo escribible y no de solo lectura. Un reemplazo de un archivo EXISTENTE bajo un directorio de sistema nunca aplica en linea: el item pasa al `plan.json` directamente (la semantica de shares del NTFS viejo no es fiable alli y el destino puede ser una imagen mapeada); un archivo nuevo (add-file) aplica en linea porque nadie puede tenerlo abierto, y un paquete mixto queda parcialmente aplicado hasta el reinicio (igual que la rama locked). Las raices incorporadas son las que permite la lista blanca del target (`Windows`, `Program Files`, `Program Files (x86)`); `LNUPDATE_SYSTEM_DIRS` (separadas por `;`) ANADE raices en produccion y solo la compilacion de prueba puede reemplazar o vaciar la lista.
- **confirm:** re-verifica el paquete y recalcula TODOS los hashes finales; solo entonces `confirmed` + `healthConfirmed`. Si falla, la historia no se reescribe: el estado sigue `applied` y se registra `failureRecorded` (salvo plan aun pendiente).
- **rollback / remove-package:** restaura desde el backup verificado, valida el camino real, comprueba el resultado; funciona con los componentes nuevos danados. `remove-package` de un paquete instalado = rollback + quitar del store (los objetos son inmutables y no se borran).
- **idempotencia:** el mismo request reanuda o devuelve el resultado; `prepared/applying` se reanuda desde `undo.json`.

## 5. Certificado de desarrollo y scripts
`New-DevSigningCert.ps1` crea `CN=Leeway LiberNT Update DEV` (RSA 3072, SHA-256, 5 anos); la clave privada solo en `C:\Users\Nicolas\.leeway-dev-signing\` (PFX + contrasena aleatoria, ACL solo del usuario), nunca en el repo ni en las VMs (se niega a usar una carpeta dentro del repo). Al repo solo el `.cer` publico (SHA-256 `829459d8db6adc0a4fe7500e0572149e1d469b6a3707cf48e374b7660d754a34`); `.gitignore` bloquea pfx/key/contrasena. No se genero ni publico clave de distribucion. `New-FixturePackage.ps1` arma y firma paquetes (manifest + `manifest.sig` + payload + catalogo v2 firmado) con interruptores para fixtures negativos.

## 6. Dependencias y limites
- Usa kernel32, advapi32 (seguridad, CryptoAPI), crypt32 y wintrust. SHA-256, JSON y UTF-8 son propios. Sin user32.
- Sin lock entre procesos sobre un mismo target (pendiente; la regla de ruta de directorios de sistema cubre el caso de los destinos del sistema). Sin GC del store. El rollback de un archivo en uso no se difiere a reinicio (plan `mode` solo `install`).
- `driverStart` es el declarado por el manifest firmado; no se contrasta con el registro del target (requiere leer el hive).
- Entre la comprobacion del camino real y el reemplazo queda una ventana que solo puede usar quien ya pueda escribir en el target (administrador).
- El aplicador de arranque (Kevin) debe verificar `plan.signature` (firma del manifest) y `package.digest` sin WinVerifyTrust; falta confirmar con Kevin los puntos PROPUESTA 1-6 de S1-CONTRACTS.md.
- Sin dependencias entre paquetes, CAB/MSU/deltas, red, kernel/SMSS.

## 7. Cambios de la revision de Pam
B1 arbol protegido + re-verificacion en apply/confirm/pending; B2 staging privado; B3 lista blanca, 8.3, dispositivos, reparse points y camino real; B4 journal estricto + ancla; N1 solo `CERT_E_UNTRUSTEDROOT`; N3 ACCESS_DENIED acotado; N4 ganchos de prueba solo con `LN_TESTING`; N5 ids sin nombres de dispositivo; N6 plan validado contra el paquete. N2 (prueba negativa del `.cat` con la firma CMS alterada en la VM de LiberNT, y SIP de `.cat` registrado) y N7 (lock, GC, vencimiento del certificado DEV en 5 anos) quedan pendientes.

## 8. Informe de COMPILACION
- **Host** (`tools/libernt-update/Build-And-Test.ps1`, clang llvm-mingw x86_64, `-Wall -Wextra`): libservicing (10 archivos) + `s1core.c`, libernt-update.exe y libservicing-tests.exe (con `-DLN_TESTING`) -> 0 avisos, `COMPILATION: OK`.
- **Contra LiberNT** (headers `sdk/include`, toolchain Clang de RosBE, amd64, `ninja -j1`, con los dos `add_subdirectory` aplicados solo en un arbol local sin commitear): `libservicing`, `libservicing_testlib`, `libernt-update` y `libservicing_tests` compilan y enlazan contra las bibliotecas de importacion de LiberNT, incluyendo `s1core.c`. Dos hallazgos que el host no daba: `GetNamedSecurityInfoW` recibe `LPWSTR` en los headers de LiberNT (cast), y `s1core.c` de Kevin dejaba un `fprintf(stderr, ...)` de depuracion dentro de `S1Rsa2048Sha256Verify` que no enlaza en LiberNT (`_iob` no existe en su msvcrt): se quitaron esas dos lineas en la copia importada; Kevin debe quitarlas tambien en su rama. `libernt-update.exe` importa solo advapi32, kernel32, msvcrt y wintrust. Esto confirma que las APIs usadas existen en LiberNT; NO prueba que se comporten igual.
- Los `CMakeLists.txt` padres no se modificaron en la rama (el nucleo sigue desconectado).

## 9. Informe de PRUEBAS
Host, Windows 11 sin elevar, wintrust real: **343 comprobaciones, 0 fallos**: SHA-256, JSON, rutas y lista blanca, esquema del manifest, transactionId, journal incl. ancla, identidad del target, arbol protegido, verificacion con `manifest.sig` real (bueno, payload alterado, manifest alterado tras firmar, firma danada, sin firma, firmado por otro certificado, destino kernel, payload ausente, catalogo opcional ausente / presente sin firma, driver con y sin catalogo), rechazo antes de staging, ciclo completo, fallo a mitad con rollback, objeto corrupto, re-verificacion en apply/confirm, copia privada ante cambio del origen, junction, plan pendiente y su validacion, reglas de intentos, export-log.
- **AddressSanitizer** (clang `-fsanitize=address`, mismo codigo): 343/0, sin errores. Encontro y corrigio un uso despues de liberar real en `LnServicingApplyPackage` (se leia `last->phase` despues de `LnJournalFree`); antes aparecia como segfault intermitente.
- **Robustez ante antivirus/indexadores:** el host de pruebas tiene escaneo en tiempo real y producia fallos transitorios (sharing violation, acceso denegado, archivo mapeado) al reemplazar un archivo recien escrito. `LnWriteFileAtomic`, la apertura del journal y el reemplazo de destinos reintentan unos instantes (mas en una imagen offline, menos en el sistema en marcha).
- No probado: VM LiberNT/dx2400, `WinVerifyTrust` de LiberNT sobre un `.cat`, target Online real, archivo realmente en uso, corte de luz real, aplicador de arranque/BootExecute, servicio, ejecucion elevada con el DACL solo SYSTEM+Administrators (los tests aceptan ademas la cuenta que los corre), `manifest.sig` verificado por el aplicador real de Kevin (comparten el codigo, no se probo en conjunto).

## 10. Integracion de s1core (Kevin, bfa62cef6d) y notas de la 2da pasada de Pam
- `base/system/servicing/pending/s1core.{c,h}` es ahora EXACTAMENTE el commit bfa62cef6d de Kevin (unica fuente; sin copia divergente; sin fprintf; pinning e==65537; helpers de prueba bajo `S1_HOST_TEST`).
- `CertRsa2048Key` exige ademas bit alto del modulo, modulo impar y e==65537 (misma politica que el core). El `.cer` y `manifest.json` se leen UNA sola vez en `VerifyCommon`; los mismos bytes se parsean, hashean y verifican.
- `apply` compara el `manifestSha256` del paquete re-verificado con el de la transaccion; `LnTargetOpen` rechaza reparse points en `LiberNT` y `LiberNT\Servicing`; `LnTargetValidateReal` se llama justo antes de cada escritura; `COM`/`LPT` con superindices U+00B9/B2/B3 son nombres reservados.
- Politica de nombres de stage 1 (documentada en `target.c`): la lista blanca dice que PUEDE reemplazarse, no que es peligroso; lsasrv, samsrv, msv1_0, sechost, svchost, kernel32, kernelbase no estan en la lista de exclusion: quien firma con la clave DEV es confiable para entregarlos; el dueno decide cuando crece esa lista.
- Pruebas negativas nuevas (29): bit flip de la firma en 4 posiciones, firma de 255/257/1 bytes, e=3, e=65539, modulo sin bit alto, modulo par, DER truncado. Host 343/0, ASan 343/0. Compilacion contra LiberNT: repetir pendiente en este commit (el anterior enlazo bien).

## Entrega por red (add-package --from)

`fetch.c` (libservicing) trae un paquete por HTTP a un directorio temporal y el flujo de
verificacion y staging sigue siendo el de siempre; el transporte no es de confianza.

- Cliente HTTP/1.0 minimo sobre ws2_32 crudo: en el build de campo probado, WinHTTP
  envia la peticion pero falla al recibir la respuesta (error 12157), asi que la
  implementacion no depende de el.
- `libernt-update add-package --from <url>`: http:// solamente, status 200 solamente,
  sin redirects, tope de 64 MiB por archivo (`LN_FETCH_MAX_BYTES`) y timeout de 30 s
  (`LN_FETCH_TIMEOUT_MS`); un archivo a medio bajar se borra siempre.
- Los nombres de los payloads salen del manifest FIRMAdo; cada payload se compara con
  su sha256 al bajar y el add path re-verifica firma y hashes con la misma autoridad
  de siempre.
- Casos de prueba en `TestFetch` (31 comprobaciones): bytes exactos, 404, tope de
  tamano, conexion rechazada, URLs rechazadas (https/userinfo/backslashes/controles/
  no-ASCII), paquete firmado end-to-end por HTTP, payload adulterado rechazado,
  firma del manifest verificada ANTES de bajar payloads (manifest adulterado /
  firma de otra clave / sin firma = 404 del .sig), path de payload de ~5000 chars
  falla limpio (review F1), tope TOTAL por paquete 256 MiB (env LN_FETCH_MAX_PACKAGE_BYTES,
  review F2). LnFetchPackage recibe el target para anclar la firma al cert pinned. El harness
  (`tools/libernt-update/Build-And-Test.ps1`) sirve los fixtures en
  127.0.0.1:8097 y exporta `LN_FETCH_TEST_URL`.
