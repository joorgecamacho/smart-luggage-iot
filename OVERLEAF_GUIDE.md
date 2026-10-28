# Guía de Uso del Paper en Overleaf

Esta guía explica paso a paso cómo abrir, editar y sincronizar el **Informe Técnico (Paper IEEE)** del proyecto **AeroTrack IoT** en **Overleaf** para el equipo:
- **Oscar Salueña Pujolràs** (NIA: 285547)
- **Jorge Camacho Camacho** (NIA: 266986)
- **Miguel Rando Ramirez** (NIA: 253182)

---

## 🚀 Método 1: Subir directamente el archivo ZIP (Recomendado, 30 segundos)

Este es el método más rápido y no requiere ninguna configuración previa:

1. Ve a tu cuenta de [Overleaf](https://www.overleaf.com/) e inicia sesión.
2. En el panel principal, haz clic en el botón verde **"New Project"** (Nuevo Proyecto) en la esquina superior izquierda.
3. Selecciona la opción **"Upload Project"** (Subir Proyecto).
4. Arrastra y suelta el archivo **`paper.zip`** que se encuentra en la raíz de este repositorio (o dentro de la carpeta `paper/`).
5. Overleaf descomprimirá automáticamente todos los archivos (`main.tex`, `IEEEtran.cls`, `references.bib` y la carpeta `figures/`) y compilará el documento automáticamente en la vista previa.

---

## 🔄 Método 2: Sincronización directa con GitHub

Si tienes cuenta de Overleaf con integración de GitHub:

1. En Overleaf, pulsa **"New Project"** → **"Import from GitHub"**.
2. Selecciona el repositorio: `joorgecamacho/smart-luggage-iot`.
3. Overleaf clonará el repositorio.
4. Cada cambio que hagáis en Overleaf podéis sincronizarlo con GitHub usando el menú **"Git"** en la barra superior de Overleaf.
5. Los cambios que se hagan en GitHub podréis descargarlos a Overleaf con el botón **"Pull changes into Overleaf"**.

---

## 📁 Estructura de Archivos del Paper

El directorio `paper/` está configurado con el estándar de congresos y revistas científicas IEEE:

```text
paper/
├── IEEEtran.cls                   # Clase oficial de formato IEEE (2 columnas)
├── main.tex                       # Documento principal del Technical Report
├── overleaf_workflow_guide.tex    # Guía en formato paper sobre el flujo de trabajo
├── references.bib                 # Base de datos de referencias BibTeX (IATA, IoT, RFID, MQTT...)
├── Makefile                       # Script de compilación para terminal
└── figures/                       # Figuras y diagramas de alta resolución
    ├── fig2_app_flowchart.pdf / .png       # Diagrama de flujo de reconciliación de bodega
    └── fig3_state_machine.pdf / .png       # Máquina de estados del firmware
```

---

## 📑 Secciones del Technical Report (según los requisitos del curso)

El archivo `main.tex` incluye los títulos exactos definidos para la entrega:

1. **Title**: `Final Technical Report: AeroTrack IoT -- Smart RFID Baggage Tracking and Automated Cargo Hold Reconciliation System`
2. **Authors**: Oscar Salueña Pujolràs (285547), Jorge Camacho Camacho (266986) y Miguel Rando Ramirez (253182).
3. **Abstract & Keywords**: Resumen técnico de alto impacto con palabras clave.
4. **Section 1: System Architecture (devices, cloud services, software modules...)**
   - *A rellenar y diseñar por el equipo*: Dispositivos, servicios cloud y módulos de software.
5. **Section 2: App design (flow diagrams, etc.)**
   - Flujo de operación paso a paso.
   - Máquina de estados finitos (FSM) de 5 estados.
6. **Section 3: Protocol description (sensor readings sent to the cloud, commands received from the cloud...)**
   - Topics MQTT y payloads JSON (ej. `aerotrack/esp32/luggage/scan`).
   - Comandos recibidos de la nube (`COMMAND_HOLD_LOCK`, `COMMAND_SYNC_MANIFEST`).
7. **Section 4: Code description**
   - Módulos de firmware: `RFID_Module`, `Sensor_Module`, `Actuator_Module`, `Cloud_Client`.
8. **Section 5: Demo**
   - Escenario de demostración en vivo (maleta correcta → LED verde + suma en nube; maleta incorrecta → LED rojo + alarma nube; potenciómetro → velocidad/compuerta).
9. **Section 6: Issues and solutions**
   - Desafíos resueltos (colisiones RFID, calibración de ADC en ESP32-S2, reconexiones MQTT).
10. **Section 7: Any other matter you want to explain**
    - Cumplimiento de la norma **IATA Resolution 753**.
    - Gestión de itinerarios multi-escala (ej. Barcelona → Berlín → Australia).
11. **Section 8: Results and Proof of Results**
    - Métricas de latencia de lectura, tiempo de respuesta a la nube y precisión de conteo (100%).
12. **Section 9: Conclusions**
    - Resumen y trabajo futuro.
13. **Section 10: References**
    - Referencias bibliográficas formateadas con `\bibliographystyle{IEEEtran}`.

---

## 👥 Consejos para trabajar los 3 en equipo en Overleaf

1. **Edición simultánea**: Overleaf permite que los 3 integrantes editen al mismo tiempo en tiempo real, viendo el cursor y las selecciones de cada uno.
2. **Comentarios y modo revisión**: Podéis seleccionar cualquier texto y añadir un comentario (`Ctrl + Shift + C` o botón de comentario en el lateral) para discutir cambios antes de modificar el texto.
3. **Añadir nuevas referencias**: Solo tenéis que abrir `references.bib`, pegar la entrada BibTeX (de Google Scholar, IEEE Xplore, etc.) y citarla en `main.tex` con `\cite{clave}`.
4. **Exportar a PDF**: Pulsad el botón azul **"Recompile"** para actualizar la vista y luego **"Download PDF"** para obtener el documento final listo para entregar.
