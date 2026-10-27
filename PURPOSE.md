# Leeway Open Software: purpose and development principles

English first, Spanish below (texto en español más abajo).

### Our purpose

**Leeway Open Software researches, develops and refines free and open-source software to turn promising technologies into complete, modern solutions that work in the real world.**

We start from existing projects, study their architectures, identify their limitations and develop our own improvements, prioritizing compatibility, stability, technological independence and the user experience.

In the field of Windows-compatible systems, we work with technologies such as LiberNT, ReactOS and Wine, taking the functional maturity of Windows 11 as our reference, without reproducing proprietary code or depending on its internal implementations.

Our goal is not simply to show that something can work. **We want it to work well, to be useful, and to be maintainable and able to evolve.**

### Development principles

**1. Reality over demonstration.**

Software must work outside controlled environments. Virtual machines make development easier, but real hardware is what validates the compatibility, stability and actual behavior of the system.

**2. Learn from standards, do not copy their implementations.**

We study how modern systems, especially Windows, Linux and macOS, solve problems. We adopt the design and experience principles that prove useful, while developing our own solutions with legitimate technologies and resources.

**3. Research before implementing.**

Technical research is part of development. Aureline and Asterion let us study architectures and behaviors; the findings ground the decisions that are later implemented and tested.

**4. Independence without isolation.**

We respect the original projects, their licenses and their communities. We keep our modifications publicly available where appropriate, without imposing our direction of development on their maintainers.

**5. Responsibility and authorship.**

Developments, research and contributions are presented under the Leeway Open Software identity, preserving the attributions and license obligations owed to third parties.

**6. Tools that solve real problems.**

We prioritize concrete functionality: hardware compatibility, drivers, installation, maintenance, recovery, diagnostics, security and productivity tools.

**7. Sustainable evolution.**

Every change must be possible to research, reproduce, test and maintain. Innovation does not justify sacrificing stability or destroying capabilities that already work.

### Current goals: Windows-compatible systems

- **LiberNT:** improve compatibility with real hardware, starting with network connectivity and the driver stack.
- **Device installation:** research and implement driver detection, installation and reconfiguration with an experience comparable to that of modern Windows systems.
- **Aureline and Asterion:** maintain a technical research environment that grounds our own implementations.
- **LibrePE / LibreRE:** develop installation, recovery, diagnostics and forensic analysis environments.
- **Validation:** use QEMU for controlled testing and the HP dx2400 as the initial physical validation platform.

**Our philosophy: research rigorously, build independently and prove results in the real world.**

---

### Nuestro propósito

**Leeway Open Software investiga, desarrolla y perfecciona software libre y de código abierto para convertir tecnologías prometedoras en soluciones completas, modernas y utilizables en el mundo real.**

Partimos de proyectos existentes, estudiamos sus arquitecturas, identificamos sus limitaciones y desarrollamos mejoras propias, priorizando la compatibilidad, la estabilidad, la independencia tecnológica y la experiencia del usuario.

En el ámbito de los sistemas compatibles con Windows, trabajamos con tecnologías como LiberNT, ReactOS y Wine, tomando la madurez funcional de Windows 11 como referencia, sin reproducir código propietario ni depender de sus implementaciones internas.

Nuestro objetivo no es simplemente demostrar que algo puede funcionar. **Queremos que funcione bien, que sea útil y que pueda mantenerse y evolucionar.**

### Principios de desarrollo

**1. La realidad por encima de la demostración.**

El software debe funcionar fuera de los entornos controlados. Las máquinas virtuales facilitan el desarrollo, pero el hardware real permite validar la compatibilidad, la estabilidad y el comportamiento efectivo del sistema.

**2. Aprender de los estándares, no copiar sus implementaciones.**

Estudiamos cómo resuelven los problemas los sistemas modernos, especialmente Windows, Linux y macOS. Adoptamos principios de diseño y experiencia que resulten útiles, desarrollando nuestras propias soluciones con tecnologías y recursos legítimos.

**3. Investigar antes de implementar.**

La investigación técnica es parte del desarrollo. Aureline y Asterion permiten estudiar arquitecturas y comportamientos; los resultados fundamentan las decisiones que posteriormente se implementan y prueban.

**4. Independencia sin aislamiento.**

Respetamos los proyectos originales, sus licencias y sus comunidades. Mantenemos nuestras modificaciones públicamente disponibles cuando corresponde, sin imponer nuestra dirección de desarrollo a sus responsables.

**5. Responsabilidad y autoría.**

Los desarrollos, investigaciones y contribuciones se presentan bajo la identidad de Leeway Open Software, conservando las atribuciones y obligaciones de licencia correspondientes a terceros.

**6. Herramientas que resuelven problemas reales.**

Priorizamos funcionalidades concretas: compatibilidad de hardware, controladores, instalación, mantenimiento, recuperación, diagnóstico, seguridad y herramientas de productividad.

**7. Evolución sostenible.**

Cada modificación debe poder investigarse, reproducirse, probarse y mantenerse. La innovación no justifica sacrificar la estabilidad ni destruir capacidades que ya funcionan.

### Objetivos actuales: sistemas compatibles con Windows

- **LiberNT:** mejorar la compatibilidad con hardware real, comenzando por la conectividad de red y la pila de controladores.
- **Instalación de dispositivos:** investigar e implementar detección, instalación y reconfiguración de drivers con una experiencia comparable a la de sistemas Windows modernos.
- **Aureline y Asterion:** mantener un entorno de investigación técnica que permita fundamentar implementaciones propias.
- **LibrePE / LibreRE:** desarrollar entornos de instalación, recuperación, diagnóstico y análisis forense.
- **Validación:** utilizar QEMU para pruebas controladas y la HP dx2400 como plataforma inicial de validación física.

**Nuestra filosofía: investigar con rigor, construir con independencia y demostrar resultados en el mundo real.**
