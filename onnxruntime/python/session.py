#-------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
#--------------------------------------------------------------------------

import sys
import os

from onnxruntime.capi import _pybind_state as C


class InferenceSession:
    """
    This is the main class used to run a model.
    """
    def __init__(self, path_or_bytes, sess_options=None):
        """
        :param path_or_bytes: filename or serialized model in a byte string
        :param sess_options: session options
        """
        self._path_or_bytes = path_or_bytes
        self._sess_options = sess_options
        self._load_model()

    def _load_model(self, providers=[]):
        if self._sess_options:
            self._sess = C.InferenceSession(
                self._sess_options, C.get_session_initializer())
        else:
            self._sess = C.InferenceSession(
                C.get_session_initializer(), C.get_session_initializer())

        if isinstance(self._path_or_bytes, str):
            self._sess.load_model(self._path_or_bytes, providers)
        elif isinstance(self._path_or_bytes, bytes):
            self._sess.read_bytes(self._path_or_bytes, providers)
        elif isinstance(self._path_or_bytes, tuple):
            # to remove, hidden trick
            self._sess.load_model_no_init(self._path_or_bytes[0], providers)
        else:
            raise TypeError("Unable to load from type '{0}'".format(type(self._path_or_bytes)))

        self._inputs_meta = self._sess.inputs_meta
        self._outputs_meta = self._sess.outputs_meta
        self._model_meta = self._sess.model_meta
        self._providers = self._sess.get_providers()

    def _reset_session(self):
        "release underlying session object."
        # meta data references session internal structures
        # so they must be set to None to decrement _sess reference count.
        self._inputs_meta = None
        self._outputs_meta = None
        self._model_meta = None
        self._providers = None
        self._sess = None

    def get_inputs(self):
        "Return the inputs metadata as a list of :class:`onnxruntime.NodeArg`."
        return self._inputs_meta

    def get_outputs(self):
        "Return the outputs metadata as a list of :class:`onnxruntime.NodeArg`."
        return self._outputs_meta

    def get_modelmeta(self):
        "Return the metadata. See :class:`onnxruntime.ModelMetadata`."
        return self._model_meta

    def get_providers(self):
        "Return list of registered execution providers."
        return self._providers

    def set_providers(self, providers):
        "Register the input list of execution providers. The underlying session is re-created."
        if not set(providers).issubset(C.get_available_providers()):
          raise ValueError("{} does not contain a subset of available providers {}".format(providers, C.get_available_providers()))
        self._reset_session()
        self._load_model(providers)

    def run(self, output_names, input_feed, run_options=None):
        """
        Compute the predictions.

        :param output_names: name of the outputs
        :param input_feed: dictionary ``{ input_name: input_value }``
        :param run_options: See :class:`onnxruntime.RunOptions`.

        ::

            sess.run([output_name], {input_name: x})
        """
        num_required_inputs = len(self._inputs_meta)
        num_inputs = len(input_feed)
        # the graph may have optional inputs used to override initializers. allow for that.
        if num_inputs < num_required_inputs:
            raise ValueError("Model requires {} inputs. Input Feed contains {}".format(num_required_inputs, num_inputs))
        if not output_names:
            output_names = [output.name for output in self._outputs_meta]
        return self._sess.run(output_names, input_feed, run_options)

    def end_profiling(self):
        """
        End profiling and return results in a file.

        The results are stored in a filename if the option
        :meth:`onnxruntime.SessionOptions.enable_profiling`.
        """
        return self._sess.end_profiling()
