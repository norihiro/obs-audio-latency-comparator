# OBS Audio Latency Comparator

## Introduction

This is a simple plugin for OBS Studio that compares latencies of two audio sources.

## Properties

### Source 1 / 2

Select audio sources that you want to compare.

### Include Sync Offset

By enabling this option, the `Sync Offset` configured at `Advanced Audio Properties` will be added.

### Window

Window to calculate the correlation of the two specified sources.
The window defines number of samples included in an isolated portion of the audio data.

### Range

Range to move the window.
The range defines the scope of data considered for correlation calculations.
