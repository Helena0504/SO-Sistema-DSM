# Guía de configuración y ejecución del sistema DSM

Este documento describe el procedimiento para iniciar y comprobar la comunicación de varios nodos del sistema DSM en equipos Linux conectados a una misma red. Todos los nodos ejecutan el mismo programa y utilizan una copia idéntica del archivo de configuración.

## 1. Requisitos

- Equipos Linux con conectividad TCP entre sí.
- Código fuente del proyecto disponible en cada equipo.
- GCC y Make instalados.
- Puertos TCP configurados para el proyecto disponibles en cada equipo.

Para instalar las herramientas de compilación, si fuera necesario:

```bash
sudo apt update
sudo apt install -y build-essential make
```

## 2. Identificación de equipos y configuración

En cada equipo se consulta su dirección IP:

```bash
ip -4 addr
```

Las direcciones de todos los participantes se registran en el archivo:

```text
sistema/sistema.conf
```

La lista se define mediante el parámetro `nodos`, con el formato `ID:IP`. Por ejemplo, para tres equipos:

```ini
nodos=0:192.168.1.101,1:192.168.1.102,2:192.168.1.103
```

Las direcciones anteriores son ilustrativas y deben sustituirse por las asignadas a los equipos utilizados. Los identificadores deben ser distintos; la implementación actual admite hasta 64 nodos, con IDs del 0 al 63.

El resto de los parámetros de red se mantiene según los puertos asignados al proyecto:

```ini
puertoBase=28000
puertoNodos=28001
puertoHeartbeat=28002
puertoPages=28003
puertoConsulta=28004

pausaH=1000
muertoH=3000
ipLocal=0.0.0.0
```

El programa determina la identidad local comparando las direcciones del equipo con las registradas en `nodos`. No es necesario modificar el código ni crear configuraciones individuales.

**El archivo `sistema.conf` debe ser idéntico en todos los equipos.** Si se agrega o elimina un participante, se actualiza la lista, se distribuye la nueva configuración y se reinician los procesos.

## 3. Compilación

Desde la raíz del proyecto, donde se encuentra el `Makefile`, ejecutar en cada equipo:

```bash
make build/dsm_nodo
```

Para verificar previamente los módulos mediante las pruebas existentes, se puede ejecutar:

```bash
make test
```

Estas pruebas locales no sustituyen la comprobación de la comunicación entre equipos físicos.

## 4. Inicio del clúster

En cada equipo, desde la raíz del proyecto, ejecutar:

```bash
./build/dsm_nodo sistema/sistema.conf
```

Cada proceso debe mostrar el identificador y la dirección IP correspondiente a su equipo. El orden de inicio no es relevante: el sistema vuelve a intentar la conexión con los nodos que todavía no estén disponibles y mantiene el intercambio de mensajes de heartbeat.

Para consultar el estado del clúster, escribir en la terminal del DSM:

```text
nodos
```

Cuando todos los participantes estén registrados, el contador debe indicar `N/N`, donde `N` es la cantidad de equipos incluidos en la configuración. Por ejemplo, para tres equipos: `Nodos conectados: 3/3`.

No es necesario ejecutar `ping` entre todas las combinaciones de computadoras. La consulta `nodos` permite comprobar el registro desde el propio sistema. Las pruebas de conectividad se reservan para casos en los que un equipo no consiga unirse.

## 5. Comprobación de transferencia de páginas

La aplicación de demostración utiliza la **página 0**. El primer identificador de la lista de configuración corresponde al nodo que inicializa su contenido; en los ejemplos siguientes se utiliza el **nodo 0** como origen.

1. **Nodo de origen (nodo 0):** ejecutar `pagina` para consultar el contenido inicial y el estado de la página.
2. **Nodo solicitante (nodo 1):** ejecutar `pedir 0` para solicitar la página al nodo 0 mediante TCP. Ejecutar `pagina` para comprobar su recepción y estado `SHARED`.
3. **Nodos adicionales:** ejecutar `pedir 0` desde los equipos que deban conservar una copia. Cada uno puede consultar su página con `pagina`.
4. **Nodo de origen:** ejecutar `modificar`. El cambio a `MODIFIED` debe denegarse mientras existan copias compartidas.
5. **Nodo de origen:** ejecutar `invalidar ID` para cada nodo que tenga una copia, sustituyendo `ID` por su identificador. Por ejemplo: `invalidar 1`, `invalidar 2`.
6. **Nodo de origen:** después de invalidar todas las copias, ejecutar `modificar`, seguido de `escribir Contenido actualizado` y `pagina`.
7. **Nodos solicitantes:** ejecutar `pagina` para comprobar que las copias invalidadas quedaron en estado `INVALID`.
8. **Cualquier nodo:** ejecutar `stats` para consultar los contadores locales de transferencias, bytes e invalidaciones.

El comando `tabla` muestra la tabla local de páginas. El comando `ayuda` presenta las opciones disponibles. Para finalizar el proceso, ejecutar `salir`.

### Comandos de la demostración

| Comando | Función |
| --- | --- |
| `nodos` | Consultar los nodos registrados y su disponibilidad. |
| `pagina` | Consultar el estado y, cuando sea válido, el contenido de la página 0. |
| `pedir ID` | Solicitar la página 0 al nodo indicado. |
| `invalidar ID` | Enviar una invalidación al nodo indicado. |
| `modificar` | Intentar obtener el estado `MODIFIED`. |
| `escribir TEXTO` | Modificar el contenido cuando se cuenta con permiso de escritura. |
| `tabla` | Mostrar la tabla local de páginas. |
| `stats` | Mostrar estadísticas locales. |
| `ayuda` | Mostrar los comandos disponibles. |
| `salir` | Finalizar la ejecución. |

## 6. Verificación de desconexión y reconexión

Con varios nodos conectados, finalizar uno de ellos mediante `salir`. Luego, desde otro nodo, ejecutar `nodos` para comprobar que se registra su desconexión una vez superado el tiempo de espera de heartbeat.

Volver a iniciar el programa en el equipo detenido y consultar nuevamente `nodos`. El sistema debe intentar registrar el nodo otra vez. Esta prueba verifica la reconexión, pero no implica recuperación de contenido perdido ni de versiones anteriores de páginas.

## 7. Diagnóstico de conexión

Si un nodo no aparece conectado:

1. Comprobar su dirección con `ip -4 addr` y verificar que coincida con la entrada correspondiente de `nodos=`.
2. Verificar que todos los equipos utilicen la misma versión de `sistema.conf`.
3. Confirmar que el ejecutable esté iniciado y que los puertos estén en escucha:

   ```bash
   ss -ltn | grep 2800
   ```

4. Desde un equipo que sí esté funcionando, comprobar la conectividad hacia el equipo que presenta el problema:

   ```bash
   ping -c 3 IP_DEL_NODO
   nc -vz IP_DEL_NODO 28001
   ```

   `IP_DEL_NODO` debe sustituirse por la dirección del equipo. `nc` requiere el paquete `netcat-openbsd` si no está instalado.
5. Revisar el firewall y las restricciones de la red del laboratorio. Si UFW está activo, se debe autorizar el tráfico TCP del rango de puertos asignado únicamente entre las máquinas participantes, conforme a las políticas de esa red.

Una respuesta fallida a `ping` no confirma por sí sola que TCP esté bloqueado; la comprobación del puerto permite distinguir ambos casos.

## 8. Alcance de la demostración técnica

El ejecutable `dsm_nodo` permite comprobar el descubrimiento de nodos, el intercambio de heartbeat, la transferencia explícita de páginas mediante TCP, las invalidaciones y las consultas locales de estado y estadísticas.

La recuperación automática de páginas remotas a partir de `SIGSEGV`, la distribución transparente de memoria para las aplicaciones y la ejecución de las dos aplicaciones de validación son funcionalidades adicionales necesarias para la demostración final solicitada en el proyecto. La prueba interactiva descrita aquí no sustituye esas validaciones.
