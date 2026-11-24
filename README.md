# PS3111 SATA Lab

Prototipo experimental para estudiar el acceso SATA directo a controladores
Phison S11/PS3111 desde Windows. Incluye un conversor SAT12/SAT16, un diagnóstico
IDENTIFY con selección explícita del disco y una biblioteca C de laboratorio.

**Estado: investigación y diagnóstico de solo lectura.** La adaptación local
consiguió comunicación SATA, consultas NAND y transferencias RAM, pero el SSD
ensayado falló una escritura sostenida. Ese resultado no demuestra una reparación
exitosa ni compatibilidad con otros SSD.

## Qué contiene

- Conversión de registros SAT a las estructuras ATA de Windows, con pruebas offline.
- CLI que, tras verificar el destino, permite exclusivamente una lectura ATA
  IDENTIFY de 512 bytes. El modo predeterminado solo muestra el plan.
- Código C x86 del puente de laboratorio y de un selector PNP para PS3111 en ROM.
  El destino nativo queda desconfigurado hasta proporcionar un ID PNP local.
- Hallazgos técnicos, un caso anonimizado y propuestas para los autores de psychite.

El paquete no incluye firmwares, MPTool/MP_API, compiladores, claves de desbloqueo,
código de psychite, volcados NAND ni información identificatoria del equipo usado.
No ofrece funciones de borrado, flasheo o escritura de datos en el diagnóstico CLI.

## Instalación y pruebas

Python 3.10 o posterior. Para las pruebas no hace falta Windows, administrador
ni un SSD conectado:

```powershell
python -m pip install -e .
python -m unittest discover -s tests -v
```

## Diagnóstico Windows

`--list` consulta metadatos de Windows; su salida puede contener números de serie.
Guardá tus configuraciones en `targets/`, que está excluida de Git.

```powershell
python -m ps3111_sata --list
```

Copiá `examples/target.example.json` a un archivo privado y reemplazá todos los
valores por el índice, ID PNP, modelo, firmware, serie y cantidad de sectores
esperados del SSD elegido. El archivo de ejemplo contiene un ID inválido a propósito.

```powershell
# Sin abrir discos: valida el archivo y muestra un plan de una sola lectura.
python -m ps3111_sata --target targets/mi-ssd.json

# Desde una consola elevada: una sola lectura, si coinciden todos los controles.
python -m ps3111_sata --target targets/mi-ssd.json --execute-identify
```

La CLI rechaza unidades de arranque/sistema, conexiones diferentes de SATA,
identidades ambiguas y cambios de destino. También comprueba los datos ATA reales.
Una respuesta incompleta o un error detienen la operación; no hay reintentos
automáticos ni selección automática de otro disco. La serie se oculta en el
resultado del diagnóstico.

## Biblioteca C y relación con MPTool

Los fuentes `native/` documentan el puente probado durante la investigación.
`SataGuard.dll` bloquea todos los dispositivos raw; `TargetSataGuard.dll` permite
solo IDENTIFY para un PS3111/SBRM02.0 en ROM con ID PNP exacto y bus SATA.

Para compilar, se probó TinyCC 0.9.27 **x86** en Windows. El compilador se consigue
por separado del proyecto TinyCC. La estructura nativa usada por estos fuentes
es de 32 bits; no compilarla como si fuera de 64 bits.

```powershell
.\native\build.ps1 -Compiler C:\ruta\al\tcc-x86\tcc.exe
```

El script ejecuta únicamente una prueba de conversión en memoria. El archivo
local `native/target_config.h` debe contener el ID PNP propio para usar el probe.
No ejecutar el probe del SSD defectuoso usado en el caso de estudio.

El puente no constituye una distribución completa de MPTool adaptado. Las
consultas VUC posteriores y la inicialización de fábrica requieren trabajo
adicional y revisión. Los programas y archivos de terceros se obtienen de sus
autores y no se redistribuyen aquí.

## Documentación

- [Resultados reales y límites](docs/case-study.md)
- [Detalles del transporte](docs/transport-findings.md)
- [Procedencia del código](docs/provenance.md)
- [Verificación del paquete](docs/validation.md)
- [Propuesta técnica para psychite](docs/upstream-transport-proposal.md)
- [Borrador sobre el recurso faltante](docs/upstream-package-data-issue.md)

## Licencia

MIT para el código original incluido en este repositorio. Las licencias de
proyectos externos y herramientas de fábrica se gestionan por separado.
