.. _gdal_raster_color_merge:

================================================================================
``gdal raster color-merge``
================================================================================

.. versionadded:: 3.12

.. only:: html

    Use a grayscale raster to replace the intensity of a RGB/RGBA dataset

.. Index:: gdal raster color-merge

Synopsis
--------

.. program-output:: gdal raster color-merge --help-doc

Description
-----------

:program:`gdal raster color-merge` allows the user to colorize a grayscale image with a RGB one.

It does the following steps:

- read the RGB (Red,Green,Blue) or RGBA (Red,Green,Blue,Alpha) dataset

- transform its RGB components into the HSV (Hue,Saturation,Value) color space

- substitute the value component with the one from the grayscale raster

- transform back to RGB.

This can be used for example to colorize a hillshade raster generated by
:ref:`gdal_raster_hillshade` with an hypsometric rendering of a DEM generated
by :ref:`gdal_raster_color_map`.

This subcommand is also available as a potential step of :ref:`gdal_raster_pipeline`

Standard options
++++++++++++++++

.. include:: gdal_options/of_raster_create_copy.rst

.. include:: gdal_options/co.rst

.. include:: gdal_options/overwrite.rst

.. option:: --input <INPUT>

   Name of the RGB/RGBA dataset. This must be a three-band or four-band Byte raster. Required

.. option:: --grayscale <GRAYSCALE>

   Name of grayscale dataset. This must be a one-band Byte raster. Required

.. GDALG output (on-the-fly / streamed dataset)
.. --------------------------------------------

.. include:: gdal_cli_include/gdalg_raster_compatible.rst

Examples
--------

.. example::
   :title: Combine a hillshade and a hypsometric rendering of a DEM into a colorized hillshade.

   .. image:: ../../images/programs/gdal_raster_color_merge/hillshade.jpg
       :alt:   Hillshade rendering of a DEM

   .. image:: ../../images/programs/gdal_raster_color_merge/hypsometric.jpg
       :alt:   Hypsometric rendering of a DEM

   .. code-block:: bash

        $ gdal raster color-merge --grayscale=hillshade.jpg hypsometric.jpg hypsometric_combined_with_hillshade.jpg


   .. image:: ../../images/programs/gdal_raster_color_merge/hypsometric_combined_with_hillshade.jpg
       :alt:   Colorized hillshade


.. below is an allow-list for spelling checker.

.. spelling:word-list::
    RGB
    RGBA
    HSV
