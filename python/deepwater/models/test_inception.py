import unittest

from deepwater.models import inception
from deepwater import optimizers

from deepwater.models.test_utils import CIFAR10_must_converge
from deepwater.models.test_utils import MNIST_must_converge
from deepwater.models.test_utils import cat_dog_mouse_must_converge

class TestInceptionV3(unittest.TestCase):

    def test_inceptionV3_cat_dog_mouse_must_converge(self):
        train_error = cat_dog_mouse_must_converge("inceptionV3", inception.InceptionV3,
                                                  optimizers.MomentumOptimizer,
                                                  batch_size=32,
                                                  epochs=500,
                                                  initial_learning_rate=1e-3,
                                                  summaries=False)
        self.assertTrue(train_error <= 0.1)

if __name__ == "__main__":
    unittest.main()
